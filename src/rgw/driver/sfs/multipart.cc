// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t
// vim: ts=8 sw=2 smarttab ft=cpp
/*
 * Ceph - scalable distributed file system
 * SFS SAL implementation
 *
 * Copyright (C) 2022 SUSE LLC
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation. See file COPYING.
 */
#include "multipart.h"

#include <fstream>

#include "rgw_sal_sfs.h"
#include "writer.h"

#define dout_subsys ceph_subsys_rgw

using namespace std;

namespace rgw::sal {

std::unique_ptr<rgw::sal::Object> SFSMultipartUpload::get_meta_obj() {
  auto mmo = std::make_unique<SFSMultipartMetaObject>(
      store, rgw_obj_key(get_meta(), string(), RGW_OBJ_NS_MULTIPART), bucket,
      bucketref
  );
  mmo->set_attrs(mp->attrs);
  mmo->set_object_ref(mp->objref);
  mp->objref->update_attrs(mp->attrs);
  return mmo;
}

int SFSMultipartUpload::init(
    const DoutPrefixProvider* dpp, optional_yield y, ACLOwner& owner,
    rgw_placement_rule& dest_placement, rgw::sal::Attrs& attrs
) {
  lsfs_dout(dpp, 10) << "owner: " << owner.get_display_name()
                     << ", attrs: " << attrs << dendl;
  lsfs_dout(dpp, 10) << "objid: " << get_key()
                     << ", upload_id: " << get_upload_id()
                     << ", meta: " << get_meta() << dendl;

  mp->init(dest_placement, attrs);
  return 0;
}

int SFSMultipartUpload::list_parts(
    const DoutPrefixProvider* dpp, CephContext* cct, int num_parts, int marker,
    int* next_marker, bool* truncated, bool assume_unsorted
) {
  lsfs_dout(dpp, 10) << "num_parts: " << num_parts << ", marker: " << marker
                     << dendl;
  ceph_assert(marker >= 0);
  ceph_assert(num_parts >= 0);

  std::map<uint32_t, std::unique_ptr<MultipartPart>> wanted;

  auto parts_map = mp->get_parts();
  uint32_t last_part_num = 0;
  for (const auto& [n, partobj] : parts_map) {
    if (n < (uint32_t)marker) {
      continue;
    }
    if (wanted.size() == (size_t)num_parts) {
      if (truncated) {
        *truncated = true;
      }
      if (next_marker) {
        *next_marker = last_part_num;
      }
      break;
    }
    wanted[n] = std::make_unique<SFSMultipartPart>(n, partobj);
    last_part_num = n;
  }

  lsfs_dout(dpp, 10) << "return " << wanted.size() << " parts of "
                     << parts_map.size() << " total, last: " << last_part_num
                     << dendl;

  parts.swap(wanted);
  return 0;
}

int SFSMultipartUpload::abort(const DoutPrefixProvider* dpp, CephContext* cct) {
  lsfs_dout(dpp, 10) << "aborting upload id " << mp->upload_id << dendl;
  if (mp->state == sfs::MultipartUpload::State::ABORTED ||
      mp->state == sfs::MultipartUpload::State::DONE) {
    return -ERR_NO_SUCH_UPLOAD;
  }
  bucketref->abort_multipart(dpp, mp->upload_id);
  return 0;
}

int SFSMultipartUpload::complete(
    const DoutPrefixProvider* dpp, optional_yield y, CephContext* cct,
    std::map<int, std::string>& part_etags,
    std::list<rgw_obj_index_key>& remove_objs, uint64_t& accounted_size,
    bool& compressed, RGWCompressionInfo& cs_info, off_t& ofs, std::string& tag,
    ACLOwner& owner, uint64_t olh_epoch, rgw::sal::Object* target_obj
) {
  lsfs_dout(dpp, 10) << "part_etags: " << part_etags
                     << ", accounted_size: " << accounted_size
                     << ", compressed: " << compressed << ", offset: " << ofs
                     << ", tag: " << tag
                     << ", owner: " << owner.get_display_name()
                     << ", epoch: " << olh_epoch
                     << ", target obj: " << target_obj->get_key()
                     << ", obj: " << mp->objref->name << dendl;

  if (mp->state == sfs::MultipartUpload::State::ABORTED) {
    lsfs_dout(dpp, 10) << "multipart with upload_id " << mp->upload_id
                       << " has been aborted." << dendl;
    return -ERR_NO_SUCH_UPLOAD;
  } else if (mp->state == sfs::MultipartUpload::State::DONE) {
    lsfs_dout(dpp, 10) << "multipart with upload_id " << mp->upload_id
                       << " has been completed." << dendl;
    return -ERR_NO_SUCH_UPLOAD;
  }

  auto parts = mp->get_parts();
  if (parts.size() != part_etags.size()) {
    return -ERR_INVALID_PART;
  }

  mp->aggregate();

  MD5 hash;

  ceph_assert(target_obj);
  ceph_assert(target_obj->get_name() == mp->objref->name);
  sfs::ObjectRef outobj = bucketref->create_version(target_obj->get_key());
  std::filesystem::path outpath =
      store->get_data_path() / outobj->get_storage_path();
  // ensure directory structure exists
  std::filesystem::create_directories(outpath.parent_path());

  ofstream out{outpath, std::ios::binary | std::ios::app};

  auto parts_it = parts.cbegin();
  auto etags_it = part_etags.cbegin();

  for (; parts_it != parts.cend() && etags_it != part_etags.cend();
       ++parts_it, ++etags_it) {
    ceph_assert(etags_it->first >= 0);
    if (parts_it->first != (uint32_t)etags_it->first) {
      // mismatch part num
      lsfs_dout(dpp, 0) << "mismatch part num, expected: " << parts_it->first
                        << ", got " << etags_it->first << dendl;
      return -ERR_INVALID_PART;
    }

    auto part = parts_it->second;
    auto part_it_etag = rgw_string_unquote(etags_it->second);
    if (part->etag != part_it_etag) {
      lsfs_dout(dpp, 0) << "mismatch part etag, expected: " << part->etag
                        << ", got " << part_it_etag << dendl;
      return -ERR_INVALID_PART;
    }

    char part_etag[CEPH_CRYPTO_MD5_DIGESTSIZE];
    hex_to_buf(part->etag.c_str(), part_etag, CEPH_CRYPTO_MD5_DIGESTSIZE);
    hash.Update((const unsigned char*)part_etag, sizeof(part_etag));

    // copy multipart object to object at offset
    std::filesystem::path partpath =
        store->get_data_path() / part->objref->path.to_path();

    ceph_assert(std::filesystem::exists(partpath));
    ceph_assert(std::filesystem::file_size(partpath) == part->len);

    lsfs_dout(dpp, 10) << "read part " << parts_it->first << " from "
                       << partpath << ", size: " << part->len << dendl;
    std::ifstream part_in{partpath, std::ios::binary};

    uint64_t read_len = 0;
    uint64_t block_size = 4194304;  // 4MB
    char* buf = (char*)std::malloc(block_size);
    while (read_len < part->len) {
      std::memset(buf, 0, block_size);
      uint64_t to_read = std::min((part->len - read_len), block_size);
      part_in.seekg(read_len);
      if (!part_in.read(buf, to_read)) {
        lsfs_dout(dpp, 0) << "error reading part " << parts_it->first
                          << " from " << partpath << ", bytes: " << to_read
                          << dendl;
        std::free(buf);
        return -ERR_INVALID_PART;
      }

      auto cur_offset = out.tellp();
      out.seekp(0);
      if (!out.write(buf, to_read)) {
        lsfs_dout(dpp, 0) << "error writing part " << parts_it->first << " to "
                          << outpath << ", bytes: " << to_read
                          << ", offset: " << cur_offset << dendl;
        std::free(buf);
        return -ERR_INVALID_PART;
      }
      read_len += to_read;
      ofs += to_read;
      accounted_size += to_read;
    }
    std::free(buf);
    lsfs_dout(dpp, 10) << "copied part " << parts_it->first
                       << ", accounted: " << accounted_size
                       << ", offset: " << ofs << dendl;
    part_in.close();
  }

  out.flush();
  out.close();

  // we are supposed to only have at most 10000 parts.
  ceph_assert(part_etags.size() <= 10000);

#define SUFFIX_LEN (6 + 1)  // '-' + len(10000) + '\0'
  char final_etag[CEPH_CRYPTO_MD5_DIGESTSIZE];
  char final_etag_str[(CEPH_CRYPTO_MD5_DIGESTSIZE * 2) + SUFFIX_LEN];
  hash.Final((unsigned char*)final_etag);
  buf_to_hex((unsigned char*)final_etag, sizeof(final_etag), final_etag_str);
  snprintf(
      &final_etag_str[CEPH_CRYPTO_MD5_DIGESTSIZE * 2],
      sizeof(final_etag_str) - (CEPH_CRYPTO_MD5_DIGESTSIZE * 2), "-%lld",
      (long long)part_etags.size()
  );
  std::string etag = final_etag_str;

  lsfs_dout(dpp, 10) << "final object " << mp->objref->name
                     << ", path: " << outpath
                     << ", accounted: " << accounted_size << ", offset: " << ofs
                     << ", etag: " << etag << dendl;

  sfs::Object::Meta meta = outobj->get_meta();
  meta.size = accounted_size;
  meta.etag = etag;
  meta.mtime = ceph::real_clock::now();
  outobj->update_meta(meta);
  outobj->update_attrs(mp->attrs);

  // remove all multipart objects. This should be done lazily in the future.
  for (const auto& [n, part] : parts) {
    std::filesystem::path partpath =
        store->get_data_path() / part->objref->path.to_path();

    ceph_assert(std::filesystem::exists(partpath));
    std::filesystem::remove(partpath);
  }
  lsfs_dout(dpp, 10) << "removed " << parts.size() << " part objects" << dendl;
  bucketref->finish_multipart(mp->upload_id, outobj);

  return 0;
}

int SFSMultipartUpload::get_info(
    const DoutPrefixProvider* dpp, optional_yield y, rgw_placement_rule** rule,
    rgw::sal::Attrs* attrs
) {
  lsfs_dout(dpp, 10) << "upload_id: " << mp->upload_id
                     << ", obj: " << mp->objref->name << dendl;

  if (rule) {
    rgw_placement_rule& placement = bucketref->get_placement_rule();
    if (!placement.empty()) {
      *rule = &placement;
    } else {
      *rule = nullptr;
    }
  }

  if (mp->state == sfs::MultipartUpload::State::NONE) {
    lsfs_dout(dpp, 10) << "upload_id: " << mp->upload_id << " does not exist!"
                       << dendl;
    return -ERR_NO_SUCH_UPLOAD;
  } else if (mp->state == sfs::MultipartUpload::State::ABORTED) {
    lsfs_dout(dpp, 10) << "upload_id: " << mp->upload_id << " has been aborted!"
                       << dendl;
    return -ERR_NO_SUCH_UPLOAD;
  }

  if (attrs) {
    *attrs = mp->objref->get_attrs();
  }

  return 0;
}

std::unique_ptr<Writer> SFSMultipartUpload::get_writer(
    const DoutPrefixProvider* dpp, optional_yield y, rgw::sal::Object* head_obj,
    const rgw_user& _owner, const rgw_placement_rule* ptail_placement_rule,
    uint64_t part_num, const std::string& part_num_str
) {
  lsfs_dout(dpp, 10) << "head obj: " << head_obj << ", owner: " << _owner
                     << ", part num: " << part_num << dendl;

  ceph_assert(head_obj->get_key().name == mp->objref->name);
  auto partref = mp->get_part(part_num);

  return std::make_unique<SFSMultipartWriter>(
      dpp, y, this, store, partref, part_num
  );
}

void SFSMultipartUpload::dump(Formatter* f) const {
  // TODO
}

}  // namespace rgw::sal
