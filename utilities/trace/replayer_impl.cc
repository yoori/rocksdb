//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#ifndef ROCKSDB_LITE

#include "utilities/trace/replayer_impl.h"

#include <cmath>
#include <thread>

#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/options.h"
#include "rocksdb/slice.h"
#include "rocksdb/status.h"
#include "rocksdb/system_clock.h"
#include "rocksdb/trace_reader_writer.h"
#include "util/threadpool_imp.h"

namespace ROCKSDB_NAMESPACE {

ReplayerImpl::ReplayerImpl(DB* db,
                           const std::vector<ColumnFamilyHandle*>& handles,
                           std::unique_ptr<TraceReader>&& reader)
    : Replayer(),
      trace_reader_(std::move(reader)),
      prepared_(false),
      trace_end_(false),
      header_ts_(0),
      exec_handler_(TraceRecord::NewExecutionHandler(db, handles)),
      env_(db->GetEnv()),
      trace_file_version_(-1) {}

ReplayerImpl::~ReplayerImpl() {
  exec_handler_.reset();
  trace_reader_.reset();
}

Status ReplayerImpl::Prepare() {
  Trace header;
  int db_version;
  Status s = ReadHeader(&header);
  if (!s.ok()) {
    return s;
  }
  s = TracerHelper::ParseTraceHeader(header, &trace_file_version_, &db_version);
  if (!s.ok()) {
    return s;
  }
  header_ts_ = header.ts;
  prepared_ = true;
  trace_end_ = false;
  return Status::OK();
}

Status ReplayerImpl::Next(std::unique_ptr<TraceRecord>* record) {
  if (!prepared_) {
    return Status::Incomplete("Not prepared!");
  }
  if (trace_end_) {
    return Status::Incomplete("Trace end.");
  }

  Trace trace;
  Status s = ReadTrace(&trace);  // ReadTrace is atomic
  // Reached the trace end.
  if (s.ok() && trace.type == kTraceEnd) {
    trace_end_ = true;
    return Status::Incomplete("Trace end.");
  }
  if (!s.ok() || record == nullptr) {
    return s;
  }

  return DecodeTraceRecord(&trace, trace_file_version_, record);
}

Status ReplayerImpl::Execute(const std::unique_ptr<TraceRecord>& record) {
  return record->Accept(exec_handler_.get());
}

Status ReplayerImpl::Execute(std::unique_ptr<TraceRecord>&& record) {
  Status s = record->Accept(exec_handler_.get());
  record.reset();
  return s;
}

Status ReplayerImpl::Replay(const ReplayOptions& options) {
  if (options.fast_forward <= 0.0) {
    return Status::InvalidArgument("Wrong fast forward speed!");
  }

  if (!prepared_) {
    return Status::Incomplete("Not prepared!");
  }
  if (trace_end_) {
    return Status::Incomplete("Trace end.");
  }

  Status s = Status::OK();

  if (options.num_threads <= 1) {
    // num_threads == 0 or num_threads == 1 uses single thread.
    std::chrono::system_clock::time_point replay_epoch =
        std::chrono::system_clock::now();

    while (s.ok()) {
      Trace trace;
      s = ReadTrace(&trace);
      // If already at trace end, ReadTrace should return Status::Incomplete().
      if (!s.ok()) {
        break;
      }

      // No need to sleep before breaking the loop if at the trace end.
      if (trace.type == kTraceEnd) {
        trace_end_ = true;
        s = Status::Incomplete("Trace end.");
        break;
      }

      // In single-threaded replay, decode first then sleep.
      std::unique_ptr<TraceRecord> record;
      s = DecodeTraceRecord(&trace, trace_file_version_, &record);
      // Skip unsupported traces, stop for other errors.
      if (s.IsNotSupported()) {
        continue;
      } else if (!s.ok()) {
        break;
      }

      std::this_thread::sleep_until(
          replay_epoch +
          std::chrono::microseconds(static_cast<uint64_t>(std::llround(
              1.0 * (trace.ts - header_ts_) / options.fast_forward))));

      s = Execute(std::move(record));
    }
  } else {
    // Multi-threaded replay.
    ThreadPoolImpl thread_pool;
    thread_pool.SetHostEnv(env_);
    thread_pool.SetBackgroundThreads(static_cast<int>(options.num_threads));

    std::mutex mtx;
    // Background decoding and execution status.
    Status bg_s = Status::OK();
    uint64_t last_err_ts = static_cast<uint64_t>(-1);
    // Callback function used in background work to update bg_s at the first
    // execution error (with the smallest Trace timestamp).
    auto error_cb = [&mtx, &bg_s, &last_err_ts](Status err, uint64_t err_ts) {
      std::lock_guard<std::mutex> gd(mtx);
      // Only record the first error.
      if (!err.ok() && !err.IsNotSupported() && err_ts < last_err_ts) {
        bg_s = err;
        last_err_ts = err_ts;
      }
    };

    std::chrono::system_clock::time_point replay_epoch =
        std::chrono::system_clock::now();

    while (bg_s.ok() && s.ok()) {
      Trace trace;
      s = ReadTrace(&trace);
      // If already at trace end, ReadTrace should return Status::Incomplete().
      if (!s.ok()) {
        break;
      }

      TraceType trace_type = trace.type;

      // No need to sleep before breaking the loop if at the trace end.
      if (trace_type == kTraceEnd) {
        trace_end_ = true;
        s = Status::Incomplete("Trace end.");
        break;
      }

      // In multi-threaded replay, sleep first thatn start decoding and
      // execution in a thread.
      std::this_thread::sleep_until(
          replay_epoch +
          std::chrono::microseconds(static_cast<uint64_t>(std::llround(
              1.0 * (trace.ts - header_ts_) / options.fast_forward))));

      if (trace_type == kTraceWrite || trace_type == kTraceGet ||
          trace_type == kTraceIteratorSeek ||
          trace_type == kTraceIteratorSeekForPrev ||
          trace_type == kTraceMultiGet) {
        std::unique_ptr<ReplayerWorkerArg> ra(new ReplayerWorkerArg);
        ra->trace_entry = std::move(trace);
        ra->handler = exec_handler_.get();
        ra->trace_file_version = trace_file_version_;
        ra->error_cb = error_cb;
        thread_pool.Schedule(&ReplayerImpl::BackgroundWork, ra.release(),
                             nullptr, nullptr);
      }
      // Skip unsupported traces.
    }

    thread_pool.WaitForJobsAndJoinAllThreads();
    if (!bg_s.ok()) {
      s = bg_s;
    }
  }

  if (s.IsIncomplete()) {
    // Reaching eof returns Incomplete status at the moment.
    // Could happen when killing a process without calling EndTrace() API.
    // TODO: Add better error handling.
    trace_end_ = true;
    return Status::OK();
  }
  return s;
}

uint64_t ReplayerImpl::GetHeaderTimestamp() const { return header_ts_; }

Status ReplayerImpl::ReadHeader(Trace* header) {
  assert(header != nullptr);
  Status s = trace_reader_->Reset();
  if (!s.ok()) {
    return s;
  }
  std::string encoded_trace;
  // Read the trace head
  s = trace_reader_->Read(&encoded_trace);
  if (!s.ok()) {
    return s;
  }

  return TracerHelper::DecodeHeader(encoded_trace, header);
}

Status ReplayerImpl::ReadFooter(Trace* footer) {
  assert(footer != nullptr);
  Status s = ReadTrace(footer);
  if (!s.ok()) {
    return s;
  }
  if (footer->type != kTraceEnd) {
    return Status::Corruption("Corrupted trace file. Incorrect footer.");
  }

  // TODO: Add more validations later
  return s;
}

Status ReplayerImpl::ReadTrace(Trace* trace) {
  assert(trace != nullptr);
  std::string encoded_trace;
  // We don't know if TraceReader is implemented thread-safe, so we protect the
  // reading trace part with a mutex. The decoding part does not need to be
  // protected since it's local.
  {
    std::lock_guard<std::mutex> guard(mutex_);
    Status s = trace_reader_->Read(&encoded_trace);
    if (!s.ok()) {
      return s;
    }
  }
  return TracerHelper::DecodeTrace(encoded_trace, trace);
}

Status ReplayerImpl::DecodeTraceRecord(Trace* trace, int trace_file_version,
                                       std::unique_ptr<TraceRecord>* record) {
  switch (trace->type) {
    case kTraceWrite:
      return TracerHelper::DecodeWriteRecord(trace, trace_file_version, record);
    case kTraceGet:
      return TracerHelper::DecodeGetRecord(trace, trace_file_version, record);
    case kTraceIteratorSeek:
    case kTraceIteratorSeekForPrev:
      return TracerHelper::DecodeIterRecord(trace, trace_file_version, record);
    case kTraceMultiGet:
      return TracerHelper::DecodeMultiGetRecord(trace, trace_file_version,
                                                record);
    case kTraceEnd:
      return Status::Incomplete("Trace end.");
    default:
      return Status::NotSupported("Unsupported trace type.");
  }
}

void ReplayerImpl::BackgroundWork(void* arg) {
  std::unique_ptr<ReplayerWorkerArg> ra(
      reinterpret_cast<ReplayerWorkerArg*>(arg));
  assert(ra != nullptr);

  std::unique_ptr<TraceRecord> record;
  Status s =
      DecodeTraceRecord(&(ra->trace_entry), ra->trace_file_version, &record);
  if (s.ok()) {
    s = record->Accept(ra->handler);
    record.reset();
  }
  if (!s.ok() && ra->error_cb) {
    ra->error_cb(s, ra->trace_entry.ts);
  }
}

}  // namespace ROCKSDB_NAMESPACE
#endif  // ROCKSDB_LITE
