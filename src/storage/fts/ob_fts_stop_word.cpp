/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "object/ob_object.h"
#define USING_LOG_PREFIX STORAGE_FTS

#include "share/rc/ob_server_runtime.h"
#include "storage/fts/ob_fts_stop_word.h"
#include "storage/fts/ob_fts_parser_helper.h"
#include "storage/fts/ob_fts_parser_property.h"

namespace oceanbase
{
namespace storage
{

namespace
{
OB_INLINE bool is_ascii_stopword(const char *s, const int64_t len)
{
  if (len <= 0 || len > 5) {
    return false;
  }
  char b[5];
  for (int64_t i = 0; i < len; ++i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c >= 0x80) {
      return false;
    }
    b[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : static_cast<char>(c);
  }
  switch (len) {
    case 1:
      return b[0] == 'a' || b[0] == 'i';
    case 2:
      switch (b[0]) {
        case 'a': return b[1] == 'n' || b[1] == 's' || b[1] == 't';
        case 'b': return b[1] == 'e' || b[1] == 'y';
        case 'd': return b[1] == 'e';
        case 'e': return b[1] == 'n';
        case 'i': return b[1] == 'n' || b[1] == 's' || b[1] == 't';
        case 'l': return b[1] == 'a';
        case 'o': return b[1] == 'f' || b[1] == 'n' || b[1] == 'r';
        case 't': return b[1] == 'o';
        default: return false;
      }
    case 3:
      return (b[0] == 't' && b[1] == 'h' && b[2] == 'e')
          || (b[0] == 'f' && b[1] == 'o' && b[2] == 'r')
          || (b[0] == 'a' && b[1] == 'r' && b[2] == 'e')
          || (b[0] == 'w' && ((b[1] == 'a' && b[2] == 's') || (b[1] == 'h' && b[2] == 'o') || (b[1] == 'w' && b[2] == 'w')))
          || (b[0] == 'c' && b[1] == 'o' && b[2] == 'm')
          || (b[0] == 'h' && b[1] == 'o' && b[2] == 'w')
          || (b[0] == 'u' && b[1] == 'n' && b[2] == 'd');
    case 4:
      return (b[0] == 't' && b[1] == 'h' && ((b[2] == 'a' && b[3] == 't') || (b[2] == 'i' && b[3] == 's')))
          || (b[0] == 'w' && ((b[1] == 'i' && ((b[2] == 't' && b[3] == 'h') || (b[2] == 'l' && b[3] == 'l')))
                           || (b[1] == 'h' && ((b[2] == 'a' && b[3] == 't') || (b[2] == 'e' && b[3] == 'n')))))
          || (b[0] == 'f' && b[1] == 'r' && b[2] == 'o' && b[3] == 'm');
    case 5:
      return (b[0] == 'a' && b[1] == 'b' && b[2] == 'o' && b[3] == 'u' && b[4] == 't')
          || (b[0] == 'w' && b[1] == 'h' && b[2] == 'e' && b[3] == 'r' && b[4] == 'e');
    default:
      return false;
  }
}

OB_INLINE bool has_non_ascii_bytes(const char *s, const int64_t len)
{
  for (int64_t i = 0; i < len; ++i) {
    if (static_cast<unsigned char>(s[i]) >= 0x80) {
      return true;
    }
  }
  return false;
}
} // anonymous namespace

////////////////////////////////////////////////////////////////////////////////
// class ObStopWordChecker
ObStopWordChecker::~ObStopWordChecker()
{
  destroy();
}

int ObStopWordChecker::init()
{
  int ret = OB_SUCCESS;

  if (inited_) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(stopword_set_.create(DEFAULT_STOPWORD_BUCKET_NUM, "StopWordSet", "StopWordSet"))) {
  } else {
    ObObjMeta stop_meta;
    stop_meta.set_varchar();
    stop_meta.set_collation_type(ObCollationType::CS_TYPE_UTF8MB4_GENERAL_CI);

    stopword_type_.set_meta(stop_meta);
    const int64_t stopword_count = sizeof(ob_stop_word_list) / sizeof(ob_stop_word_list[0]);
    for (int64_t i = 0; OB_SUCC(ret) && i < stopword_count; ++i) {
      ObFTWord stopword(STRLEN(ob_stop_word_list[i]), ob_stop_word_list[i], stopword_type_);
      if (OB_FAIL(stopword_set_.set_refactored(stopword))) {
      }
    }

    if (OB_SUCC(ret)) {
      inited_ = true;
    }
  }
  return ret;
}

void ObStopWordChecker::destroy()
{
  if (inited_) {
    stopword_set_.destroy();
    inited_ = false;
  }
}

int ObStopWordChecker::check_stopword(const ObFTWord &word, bool &is_stopword)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(word.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const common::ObString src_str = word.get_word().get_string();
    const ObCollationType src_coll = word.get_collation_type();
    const ObCharsetType cs_type = common::ObCharset::charset_type_by_coll(src_coll);
    if (OB_LIKELY(cs_type == CHARSET_UTF8MB4 || cs_type == CHARSET_ASCII || cs_type == CHARSET_LATIN1)) {
      if (src_str.length() > 10) {
        is_stopword = false;
        return OB_SUCCESS;
      }
      if (!has_non_ascii_bytes(src_str.ptr(), src_str.length())) {
        is_stopword = is_ascii_stopword(src_str.ptr(), src_str.length());
        return OB_SUCCESS;
      }
      if (cs_type == CHARSET_UTF8MB4 && static_cast<unsigned char>(src_str.ptr()[0]) >= 0xE0) {
        is_stopword = false;
        return OB_SUCCESS;
      }
    }
    common::ObArenaAllocator allocator(lib::ObMemAttr("ChkStopWord"));
    common::ObString cmp_str;
    // do nothing set out with in if type is the same.
    if (OB_FAIL(common::ObCharset::charset_convert(
                                       allocator,
                                       src_str,
                                       src_coll,
                                       stopword_type_.get_collation_type(),
                                       cmp_str))) {
    } else {
      ObFTWord converted(cmp_str.length(), cmp_str.ptr(), stopword_type_);
      ret = stopword_set_.exist_refactored(converted);
      if (OB_HASH_NOT_EXIST == ret) {
        is_stopword = false;
        ret = OB_SUCCESS;
      } else if (OB_HASH_EXIST == ret) {
        is_stopword = true;
        ret = OB_SUCCESS;
      } else if (OB_SUCC(ret)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
      }
    }
  }
  return ret;
}

////////////////////////////////////////////////////////////////////////////////
// class ObAddWord
ObAddWord::ObAddWord(
    const ObFTParserProperty &property,
    const ObObjMeta &meta,
    const ObAddWordFlag &flag,
    common::ObIAllocator &allocator,
    ObFTWordMap &word_map)
  : word_meta_(meta),
    cs_info_(common::ObCharset::get_charset(meta.get_collation_type())),
    allocator_(allocator),
    word_map_(word_map),
    min_max_word_cnt_(0),
    non_stopword_cnt_(0),
    stopword_cnt_(0),
    min_token_size_(property.min_token_size_),
    max_token_size_(property.max_token_size_),
    flag_(flag)
{
}

int ObAddWord::process_word(
    const char *word,
    const int64_t word_len,
    const int64_t char_cnt,
    const int64_t word_freq)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(word) || OB_UNLIKELY(0 >= word_len || 0 >= char_cnt || 0 >= word_freq)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (is_min_max_word(char_cnt)) {
    ++min_max_word_cnt_;
  } else {
    char stack_buf[512];
    const char *lower_ptr = word;
    int64_t lower_len = word_len;
    bool owned_by_allocator = false;

    if (flag_.casedown()) {
      bool fast_done = false;
      if (OB_LIKELY(cs_info_ != nullptr && cs_info_->casedn_multiply == 1 && word_len <= static_cast<int64_t>(sizeof(stack_buf)))) {
        const ObCharsetType cs_type = common::ObCharset::charset_type_by_coll(word_meta_.get_collation_type());
        if (OB_LIKELY(cs_type == CHARSET_UTF8MB4)) {
          bool all_safe = true;
          bool has_upper = false;
          for (int64_t i = 0; i < word_len; ) {
            const unsigned char c0 = static_cast<unsigned char>(word[i]);
            if (c0 < 0x80) {
              if (c0 >= 'A' && c0 <= 'Z') {
                has_upper = true;
                stack_buf[i] = static_cast<char>(c0 + ('a' - 'A'));
              } else {
                stack_buf[i] = static_cast<char>(c0);
              }
              ++i;
            } else if (c0 >= 0xE4 && c0 <= 0xE9 && i + 2 < word_len) {
              stack_buf[i] = word[i];
              stack_buf[i + 1] = word[i + 1];
              stack_buf[i + 2] = word[i + 2];
              i += 3;
            } else {
              all_safe = false;
              break;
            }
          }
          if (all_safe) {
            if (has_upper) {
              lower_ptr = stack_buf;
            }
            fast_done = true;
          } else {
            MEMCPY(stack_buf, word, word_len);
            lower_len = cs_info_->cset->casedn(cs_info_, stack_buf, word_len, stack_buf, sizeof(stack_buf));
            lower_ptr = stack_buf;
            fast_done = true;
          }
        }
      }
      if (!fast_done) {
        ObString dst_str;
        if (OB_FAIL(ObCharset::tolower(
                        word_meta_.get_collation_type(),
                        ObString(word_len, word),
                        dst_str,
                        allocator_))) {
          return ret;
        }
        lower_ptr = dst_str.ptr();
        lower_len = dst_str.length();
        owned_by_allocator = true;
      }
    }

    ObFTWord lookup_word(lower_len, lower_ptr, word_meta_);
    bool is_stopword = false;
    if (flag_.stopword()) {
      if (OB_FAIL(check_stopword(lookup_word, is_stopword))) {
        return ret;
      } else if (OB_UNLIKELY(is_stopword)) {
        ++stopword_cnt_;
        return OB_SUCCESS;
      }
    }

    if (!flag_.groupby_word()) {
      const char *key_ptr = lower_ptr;
      if (!owned_by_allocator) {
        char *buf = static_cast<char *>(allocator_.alloc(lower_len));
        if (OB_ISNULL(buf)) {
          return OB_ALLOCATE_MEMORY_FAILED;
        }
        MEMCPY(buf, lower_ptr, lower_len);
        key_ptr = buf;
      }
      ObFTWord insert_word(lower_len, key_ptr, word_meta_);
      if (OB_FAIL(word_map_.set_refactored(insert_word, 1/*word count*/))) {
        return ret;
      }
    } else {
      int64_t *cnt_ptr = word_map_.get(lookup_word);
      if (nullptr != cnt_ptr) {
        *cnt_ptr += word_freq;
      } else {
        const char *key_ptr = lower_ptr;
        if (!owned_by_allocator) {
          char *buf = static_cast<char *>(allocator_.alloc(lower_len));
          if (OB_ISNULL(buf)) {
            return OB_ALLOCATE_MEMORY_FAILED;
          }
          MEMCPY(buf, lower_ptr, lower_len);
          key_ptr = buf;
        }
        ObFTWord insert_word(lower_len, key_ptr, word_meta_);
        if (OB_FAIL(word_map_.set_refactored(insert_word, 1/*word count*/, 1/*overwrite*/))) {
          return ret;
        }
      }
    }
    non_stopword_cnt_ += word_freq;
  }
  return ret;
}

bool ObAddWord::is_min_max_word(const int64_t c_len) const
{
  return flag_.min_max_word() && (c_len < min_token_size_ || c_len > max_token_size_);
}

int ObAddWord::casedown_word(const ObFTWord &src, ObFTWord &dst)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(src.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (flag_.casedown()) {
    ObString dst_str;
    if (OB_FAIL(ObCharset::tolower(
                    word_meta_.get_collation_type(),
                    src.get_word().get_string(),
                    dst_str,
                    allocator_))) {
    } else {
      ObFTWord tmp(dst_str.length(), dst_str.ptr(), word_meta_);
      dst = tmp;
    }
  } else {
    dst = src;
  }
  return ret;
}

int ObAddWord::check_stopword(const ObFTWord &ft_word, bool &is_stopword)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(ft_word.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (flag_.stopword()) {
    ObStopWordChecker *stop_word_checker = ObFTParseData::instance().stop_word_checker();
    if (OB_ISNULL(stop_word_checker)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(stop_word_checker->check_stopword(ft_word, is_stopword))) {
    }
  }
  return ret;
}

int ObAddWord::groupby_word(const ObFTWord &word, const int64_t word_freq)
{
  int ret = OB_SUCCESS;
  int64_t word_count = 0;
  if (OB_UNLIKELY(word.empty() || word_freq <= 0)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (!flag_.groupby_word()) {
    if (OB_FAIL(word_map_.set_refactored(word, 1/*word count*/))) {
    }
  } else if (OB_FAIL(word_map_.get_refactored(word, word_count)) && OB_HASH_NOT_EXIST != ret) {
  } else {
    if (OB_HASH_NOT_EXIST == ret) {
      word_count = 1;
    } else {
      word_count += word_freq;
    }
    if (OB_FAIL(word_map_.set_refactored(word, word_count, 1/*overwrite*/))) {
    }
  }
  return ret;
}

} // end namespace storage
} // end namespace oceanbase
