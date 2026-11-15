/*
 * Copyright (c) 2026 OceanBase.
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

#ifndef OCEANBASE_LIB_VECTOR_CAGRA_SEARCH_WORKER_H_
#define OCEANBASE_LIB_VECTOR_CAGRA_SEARCH_WORKER_H_

#include <condition_variable>
#include <mutex>
#include <pthread.h>

namespace oceanbase {
namespace common {
namespace obvsag {

// cuVS needs a larger stack than a seekdb SQL worker has.
class CagraSearchWorker
{
public:
  using Job = void *(*)(void *);

  CagraSearchWorker()
  {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) == 0) {
      if (pthread_attr_setstacksize(&attr, 32UL * 1024 * 1024) == 0) {
        started_ = pthread_create(&thread_, &attr, &CagraSearchWorker::work, this) == 0;
      }
      pthread_attr_destroy(&attr);
    }
  }

  ~CagraSearchWorker()
  {
    if (started_) {
      {
        std::lock_guard<std::mutex> lock(mu_);
        stopping_ = true;
      }
      ready_.notify_one();
      pthread_join(thread_, nullptr);
    }
  }

  // The caller may keep the argument and its outputs on its own stack.
  bool run(Job job, void *arg)
  {
    if (!started_) {
      return false;
    }
    std::lock_guard<std::mutex> submit_lock(submit_mu_);
    std::unique_lock<std::mutex> lock(mu_);
    arg_ = arg;
    job_ = job;
    ready_.notify_one();
    finished_.wait(lock, [this] { return job_ == nullptr; });
    return true;
  }

private:
  static void *work(void *context)
  {
    auto &self = *static_cast<CagraSearchWorker *>(context);
    std::unique_lock<std::mutex> lock(self.mu_);
    for (;;) {
      self.ready_.wait(lock, [&self] { return self.job_ != nullptr || self.stopping_; });
      if (self.stopping_) {
        return nullptr;
      }
      self.job_(self.arg_);
      self.arg_ = nullptr;
      self.job_ = nullptr;
      self.finished_.notify_one();
    }
  }

  std::mutex submit_mu_;
  std::mutex mu_;
  std::condition_variable ready_;
  std::condition_variable finished_;
  Job job_ = nullptr;
  void *arg_ = nullptr;
  bool stopping_ = false;
  pthread_t thread_{};
  bool started_ = false;
};

} // namespace obvsag
} // namespace common
} // namespace oceanbase

#endif
