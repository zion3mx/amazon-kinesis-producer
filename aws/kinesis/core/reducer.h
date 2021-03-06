// Copyright 2015 Amazon.com, Inc. or its affiliates. All Rights Reserved.
//
// Licensed under the Amazon Software License (the "License").
// You may not use this file except in compliance with the License.
// A copy of the License is located at
//
//  http://aws.amazon.com/asl
//
// or in the "license" file accompanying this file. This file is distributed
// on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
// express or implied. See the License for the specific language governing
// permissions and limitations under the License.

#ifndef AWS_KINESIS_CORE_REDUCER_H_
#define AWS_KINESIS_CORE_REDUCER_H_

#include <mutex>

#include <aws/utils/logging.h>

#include <aws/utils/executor.h>
#include <aws/mutex.h>

namespace aws {
namespace kinesis {
namespace core {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

// Reduces multiple instances of input type T into an instance of output of type
// U.
//
// T and U must both meet the contracts of TimeSensitive; U must meet the
// contracts of SerializableContainer.
//
// Output can either be given as the return value of the add() method, or
// asynchronously through the deadline_callback. The callback is invoked when
// an item has reached its deadline and a flush is triggered as a result.
//
// The Reducer requires an EventBaseWorker for timing. Many reducers can share
// an instance of EventBaseWorker, but you should probably distribute reducers
// evenly among workers if you have many. This is because the timeout performs
// a flush, which is a fair amount of compute. If you're doing expensive
// operations (like SigV4) in the callback, then this becomes even more
// imperative. It fact, in those cases it's probably better to use the callback
// to redistribute the work among your workers than to do the work directly.
//
// All methods are threadsafe.
template <typename T, typename U>
class Reducer : boost::noncopyable {
 public:
  using FlushPredicate = std::function<bool (const std::shared_ptr<T>&)>;

  Reducer(
      const std::shared_ptr<aws::utils::Executor>& executor,
      const std::function<void (std::shared_ptr<U>)>& flush_callback,
      size_t size_limit,
      size_t count_limit,
      FlushPredicate flush_predicate = [](auto) { return false; })
      : executor_(executor),
        flush_callback_(flush_callback),
        size_limit_(size_limit),
        count_limit_(count_limit),
        flush_predicate_(flush_predicate),
        container_(std::make_shared<U>()),
        scheduled_callback_(
            executor_->schedule(
                [this] { this->deadline_reached(); },
                TimePoint::max())) {}

  // Put a record. If this triggers a flush, an instance of U will be returned,
  // otherwise null will be returned.
  std::shared_ptr<U> add(const std::shared_ptr<T>& input) {
    Lock lock(lock_);

    container_->add(input);

    auto size = container_->size();
    auto estimated_size = container_->estimated_size();
    auto flush_predicate_result = flush_predicate_(input);
    if (size >= count_limit_ ||
        estimated_size >= size_limit_ ||
        flush_predicate_result) {

      auto output = flush(lock);
      if (output && output->size() > 0) {
        return output;
      }
    }

    set_deadline();

    return std::shared_ptr<U>();
  }

  // Manually trigger a flush, as though a deadline has been reached
  void flush() {
    deadline_reached();
  }

  // Records in the process of being flushed won't be counted
  size_t size() const {
    return container_->size();
  }

  TimePoint deadline() const noexcept {
    if (!scheduled_callback_->completed()) {
      return scheduled_callback_->expiration();
    } else {
      return TimePoint::max();
    }
  }

 protected:
  using Mutex = aws::mutex;
  using Lock = aws::unique_lock<Mutex>;

  std::shared_ptr<U> flush(Lock& lock) {
    if (!lock) {
      lock.lock();
    }

    scheduled_callback_->cancel();

    std::vector<std::shared_ptr<T>> records(container_->items());
    container_ = std::make_shared<U>();

    lock.unlock();

    std::sort(records.begin(),
              records.end(),
              [](auto& a, auto& b) {
                return a->deadline() < b->deadline();
              });

    auto flush_container = std::make_shared<U>();
    for (auto& r : records) {
      flush_container->add(r);
    }
    records.clear();

    // TODO change to a binary search
    while ((flush_container->size() > count_limit_ ||
            flush_container->accurate_size() > size_limit_) &&
           flush_container->size() > 1) {
      records.push_back(flush_container->remove_last());
    }

    lock.lock();

    for (auto& r : records) {
      container_->add(r);
    }

    set_deadline();

    return flush_container;
  }

  void set_deadline() {
    if (container_->empty()) {
      scheduled_callback_->cancel();
      return;
    }

    if (scheduled_callback_->completed() ||
        container_->deadline() < scheduled_callback_->expiration()) {
      scheduled_callback_->reschedule(container_->deadline());
    }
  }

  void deadline_reached() {
    Lock lock(lock_);
    auto r = flush(lock);
    lock.unlock();
    if (r && r->size() > 0) {
      flush_callback_(r);
    }
  }

  std::shared_ptr<aws::utils::Executor> executor_;
  std::function<void (std::shared_ptr<U>)> flush_callback_;
  const size_t size_limit_;
  const size_t count_limit_;
  FlushPredicate flush_predicate_;
  Mutex lock_;
  std::shared_ptr<U> container_;
  std::shared_ptr<aws::utils::ScheduledCallback> scheduled_callback_;
};

} //namespace core
} //namespace kinesis
} //namespace aws

#endif //AWS_KINESIS_CORE_REDUCER_H_
