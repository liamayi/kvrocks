/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */

#include "indexer.h"

#include <algorithm>
#include <variant>

#include "db_util.h"
#include "parse_util.h"
#include "search/search_encoding.h"
#include "storage/redis_metadata.h"
#include "storage/storage.h"
#include "string_util.h"
#include "types/redis_hash.h"

namespace redis {

StatusOr<FieldValueRetriever> FieldValueRetriever::Create(IndexOnDataType type, std::string_view key,
                                                          engine::Storage *storage, const std::string &ns) {
  if (type == IndexOnDataType::HASH) {
    Hash db(storage, ns);
    std::string ns_key = db.AppendNamespacePrefix(key);
    HashMetadata metadata(false);
    auto s = db.GetMetadata(Database::GetOptions{}, ns_key, &metadata);
    if (!s.ok()) return {Status::NotOK, s.ToString()};
    return FieldValueRetriever(db, metadata, key);
  } else if (type == IndexOnDataType::JSON) {
    Json db(storage, ns);
    std::string ns_key = db.AppendNamespacePrefix(key);
    JsonMetadata metadata(false);
    JsonValue value;
    auto s = db.read(ns_key, &metadata, &value);
    if (!s.ok()) return {Status::NotOK, s.ToString()};
    return FieldValueRetriever(value);
  } else {
    assert(false && "unreachable code: unexpected IndexOnDataType");
    __builtin_unreachable();
  }
}

rocksdb::Status FieldValueRetriever::Retrieve(std::string_view field, std::string *output) {
  if (std::holds_alternative<HashData>(db)) {
    auto &[hash, metadata, key] = std::get<HashData>(db);
    std::string ns_key = hash.AppendNamespacePrefix(key);
    LatestSnapShot ss(hash.storage_);
    rocksdb::ReadOptions read_options;
    read_options.snapshot = ss.GetSnapShot();
    std::string sub_key = InternalKey(ns_key, field, metadata.version, hash.storage_->IsSlotIdEncoded()).Encode();
    return hash.storage_->Get(read_options, sub_key, output);
  } else if (std::holds_alternative<JsonData>(db)) {
    auto &value = std::get<JsonData>(db);
    auto s = value.Get(field.front() == '$' ? field : fmt::format("$.{}", field));
    if (!s.IsOK()) return rocksdb::Status::Corruption(s.Msg());
    if (s->value.size() != 1)
      return rocksdb::Status::NotFound("json value specified by the field (json path) should exist and be unique");
    *output = s->value[0].as_string();
    return rocksdb::Status::OK();
  } else {
    __builtin_unreachable();
  }
}

StatusOr<IndexUpdater::FieldValues> IndexUpdater::Record(std::string_view key) const {
  const auto &ns = info->ns;
  Database db(indexer->storage, ns);

  RedisType type = kRedisNone;
  auto s = db.Type(key, &type);
  if (!s.ok()) return {Status::NotOK, s.ToString()};

  // key not exist
  if (type == kRedisNone) return FieldValues();

  if (type != static_cast<RedisType>(info->metadata.on_data_type)) {
    // not the expected type, stop record
    return {Status::TypeMismatched};
  }

  auto retriever = GET_OR_RET(FieldValueRetriever::Create(info->metadata.on_data_type, key, indexer->storage, ns));

  FieldValues values;
  for (const auto &[field, i] : info->fields) {
    if (i.metadata->noindex) {
      continue;
    }

    std::string value;
    auto s = retriever.Retrieve(field, &value);
    if (s.IsNotFound()) continue;
    if (!s.ok()) return {Status::NotOK, s.ToString()};

    values.emplace(field, value);
  }

  return values;
}

Status IndexUpdater::UpdateTagIndex(std::string_view key, std::string_view original, std::string_view current,
                                    const SearchKey &search_key, const TagFieldMetadata *tag) const {
  const char delim[] = {tag->separator, '\0'};
  auto original_tags = util::Split(original, delim);
  auto current_tags = util::Split(current, delim);

  auto to_tag_set = [](const std::vector<std::string> &tags, bool case_sensitive) -> std::set<std::string> {
    if (case_sensitive) {
      return {tags.begin(), tags.end()};
    } else {
      std::set<std::string> res;
      std::transform(tags.begin(), tags.end(), std::inserter(res, res.begin()), util::ToLower);
      return res;
    }
  };

  std::set<std::string> tags_to_delete = to_tag_set(original_tags, tag->case_sensitive);
  std::set<std::string> tags_to_add = to_tag_set(current_tags, tag->case_sensitive);

  for (auto it = tags_to_delete.begin(); it != tags_to_delete.end();) {
    if (auto jt = tags_to_add.find(*it); jt != tags_to_add.end()) {
      it = tags_to_delete.erase(it);
      tags_to_add.erase(jt);
    } else {
      ++it;
    }
  }

  if (tags_to_add.empty() && tags_to_delete.empty()) {
    // no change, skip index updating
    return Status::OK();
  }

  auto *storage = indexer->storage;
  auto batch = storage->GetWriteBatchBase();
  auto cf_handle = storage->GetCFHandle(ColumnFamilyID::Search);

  for (const auto &tag : tags_to_delete) {
    auto index_key = search_key.ConstructTagFieldData(tag, key);

    batch->Delete(cf_handle, index_key);
  }

  for (const auto &tag : tags_to_add) {
    auto index_key = search_key.ConstructTagFieldData(tag, key);

    batch->Put(cf_handle, index_key, Slice());
  }

  auto s = storage->Write(storage->DefaultWriteOptions(), batch->GetWriteBatch());
  if (!s.ok()) return {Status::NotOK, s.ToString()};
  return Status::OK();
}

Status IndexUpdater::UpdateNumericIndex(std::string_view key, std::string_view original, std::string_view current,
                                        const SearchKey &search_key, const NumericFieldMetadata *num) const {
  auto *storage = indexer->storage;
  auto batch = storage->GetWriteBatchBase();
  auto cf_handle = storage->GetCFHandle(ColumnFamilyID::Search);

  if (!original.empty()) {
    auto original_num = GET_OR_RET(ParseFloat(std::string(original.begin(), original.end())));
    auto index_key = search_key.ConstructNumericFieldData(original_num, key);

    batch->Delete(cf_handle, index_key);
  }

  if (!current.empty()) {
    auto current_num = GET_OR_RET(ParseFloat(std::string(current.begin(), current.end())));
    auto index_key = search_key.ConstructNumericFieldData(current_num, key);

    batch->Put(cf_handle, index_key, Slice());
  }

  auto s = storage->Write(storage->DefaultWriteOptions(), batch->GetWriteBatch());
  if (!s.ok()) return {Status::NotOK, s.ToString()};
  return Status::OK();
}

Status IndexUpdater::UpdateIndex(const std::string &field, std::string_view key, std::string_view original,
                                 std::string_view current) const {
  if (original == current) {
    // the value of this field is unchanged, no need to update
    return Status::OK();
  }

  auto iter = info->fields.find(field);
  if (iter == info->fields.end()) {
    return {Status::NotOK, "No such field to do index updating"};
  }

  auto *metadata = iter->second.metadata.get();
  SearchKey search_key(info->ns, info->name, field);
  if (auto tag = dynamic_cast<TagFieldMetadata *>(metadata)) {
    GET_OR_RET(UpdateTagIndex(key, original, current, search_key, tag));
  } else if (auto numeric [[maybe_unused]] = dynamic_cast<NumericFieldMetadata *>(metadata)) {
    GET_OR_RET(UpdateNumericIndex(key, original, current, search_key, numeric));
  } else {
    return {Status::NotOK, "Unexpected field type"};
  }

  return Status::OK();
}

Status IndexUpdater::Update(const FieldValues &original, std::string_view key) const {
  auto current = GET_OR_RET(Record(key));

  for (const auto &[field, i] : info->fields) {
    if (i.metadata->noindex) {
      continue;
    }

    std::string_view original_val, current_val;

    if (auto it = original.find(field); it != original.end()) {
      original_val = it->second;
    }
    if (auto it = current.find(field); it != current.end()) {
      current_val = it->second;
    }

    GET_OR_RET(UpdateIndex(field, key, original_val, current_val));
  }

  return Status::OK();
}

Status IndexUpdater::Build() const {
  auto storage = indexer->storage;
  util::UniqueIterator iter(storage, storage->DefaultScanOptions(), ColumnFamilyID::Metadata);

  for (const auto &prefix : info->prefixes) {
    auto ns_key = ComposeNamespaceKey(info->ns, prefix, storage->IsSlotIdEncoded());
    for (iter->Seek(ns_key); iter->Valid(); iter->Next()) {
      if (!iter->key().starts_with(ns_key)) {
        break;
      }

      auto [_, key] = ExtractNamespaceKey(iter->key(), storage->IsSlotIdEncoded());

      auto s = Update({}, key.ToStringView());
      if (s.Is<Status::TypeMismatched>()) continue;
      if (!s.OK()) return s;
    }
  }

  return Status::OK();
}

void GlobalIndexer::Add(IndexUpdater updater) {
  updater.indexer = this;
  for (const auto &prefix : updater.info->prefixes) {
    prefix_map.insert(ComposeNamespaceKey(updater.info->ns, prefix, false), updater);
  }
  updater_list.push_back(updater);
}

StatusOr<GlobalIndexer::RecordResult> GlobalIndexer::Record(std::string_view key, const std::string &ns) {
  if (updater_list.empty()) {
    return Status::NoPrefixMatched;
  }

  auto iter = prefix_map.longest_prefix(ComposeNamespaceKey(ns, key, false));
  if (iter != prefix_map.end()) {
    auto updater = iter.value();
    return RecordResult{updater, std::string(key.begin(), key.end()), GET_OR_RET(updater.Record(key))};
  }

  return {Status::NoPrefixMatched};
}

Status GlobalIndexer::Update(const RecordResult &original) {
  return original.updater.Update(original.fields, original.key);
}

}  // namespace redis
