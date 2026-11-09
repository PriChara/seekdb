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

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/ik/ob_ik_cjk_processor.h"

#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "storage/fts/dict/ob_ft_dict.h"
#include "storage/fts/ik/ob_ik_char_util.h"
#include "storage/fts/ik/ob_ik_token.h"

namespace oceanbase
{
namespace storage
{
int ObIKCJKProcessor::do_process(TokenizeContext &ctx,
                                 const char *ch,
                                 const uint8_t char_len,
                                 const ObFTCharUtil::CharType type)
{
  int ret = OB_SUCCESS;

  if (ObFTCharUtil::CharType::USELESS != type) {
    const ObString character(char_len, ch);
    // Retain surviving dictionary prefixes in their original order.
    int64_t active_count = 0;
    for (int64_t i = 0; OB_SUCC(ret) && i < hits_.count(); ++i) {
      ObDATrieHit &hit = hits_[i];
      if (OB_FAIL(dict_main_.match_with_hit(character, hit, hit))) {
      } else if (hit.is_match()
                 && OB_FAIL(ctx.add_token(ctx.fulltext(),
                                          hit.start_pos_,
                                          hit.end_pos_ - hit.start_pos_,
                                          hit.char_cnt_,
                                          ObIKTokenType::IK_CHINESE_TOKEN))) {
      } else if (hit.is_match() || hit.is_prefix()) {
        if (active_count != i) {
          hits_[active_count] = hit;
        }
        ++active_count;
      }
    }
    if (OB_SUCC(ret)) {
      while (hits_.count() > active_count) {
        hits_.pop_back();
      }
      ObDATrieHit hit(&dict_main_, ctx.get_cursor());
      if (OB_FAIL(dict_main_.match(character, hit))) {
      } else if ((hit.is_match() || hit.is_prefix()) && OB_FAIL(hits_.push_back(hit))) {
      } else if (hit.is_match()
                 && OB_FAIL(ctx.add_token(ctx.fulltext(),
                                          ctx.get_cursor(),
                                          char_len,
                                          1,
                                          ObIKTokenType::IK_CHINESE_TOKEN))) {
      }
    }
  }
  if (OB_FAIL(ret) || ObFTCharUtil::CharType::USELESS == type || ctx.is_last()) {
    hits_.reuse();
  }
  return ret;
}

} //  namespace storage
} //  namespace oceanbase
