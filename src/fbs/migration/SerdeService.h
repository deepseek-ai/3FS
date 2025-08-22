#pragma once

#include "common/serde/Serde.h"
#include "common/serde/Service.h"
#include "common/utils/UtcTime.h"
#include "common/utils/Uuid.h"

namespace hf3fs::migration {

/* job state transition
  NotSubmitted --> Pending --> Running --> Succeeded
                    |           ^  ^
                    |           |  |
                    |  +--------+  |
                    |  |           |
                    v  v           v
                   Stopped     Failed
 */

enum JobStatus {
  NotSubmitted = 0,
  Pending,
  Running,
  Failed,
  Stopped,
  Succeeded,
};

struct StartJobReq {
  SERDE_STRUCT_FIELD(id,
                     Uuid::zero());  // start a new job if not found at server side; otherwise resume an existing job
  SERDE_STRUCT_FIELD(path, String{});
  SERDE_STRUCT_FIELD(mtime, UtcTime{});  // only files with mtime greater than this value
};

struct StartJobRsp {
  SERDE_STRUCT_FIELD(status, JobStatus::NotSubmitted);
};

struct StopJobReq {
  SERDE_STRUCT_FIELD(id, Uuid::zero());
};

struct StopJobRsp {
  SERDE_STRUCT_FIELD(status, JobStatus::NotSubmitted);
};

struct JobInfo {
  SERDE_STRUCT_FIELD(id, Uuid::zero());
  SERDE_STRUCT_FIELD(path, String{});
  SERDE_STRUCT_FIELD(mtime, UtcTime{});  // only files with mtime greater than this value
  SERDE_STRUCT_FIELD(status, JobStatus::NotSubmitted);
  SERDE_STRUCT_FIELD(startTime, UtcTime{});
  SERDE_STRUCT_FIELD(finishTime, UtcTime{});
  SERDE_STRUCT_FIELD(numFilesFound, uint64_t{});
  SERDE_STRUCT_FIELD(numFilesCopied, uint64_t{});
  SERDE_STRUCT_FIELD(numSrcFilesRemoved, uint64_t{});
  SERDE_STRUCT_FIELD(bytesOfFilesFound, uint64_t{});
  SERDE_STRUCT_FIELD(bytesOfFilesCopied, uint64_t{});
};

struct ListJobsReq {
  SERDE_STRUCT_FIELD(path, String{});
  SERDE_STRUCT_FIELD(ids, std::vector<Uuid>{});    // only jobs with specific ids
  SERDE_STRUCT_FIELD(status, JobStatus::Running);  // only jobs with this status
  SERDE_STRUCT_FIELD(after, UtcTime{});            // only jobs with start time greater than this value
  SERDE_STRUCT_FIELD(limit, uint32_t{10});         // max number of jobs in response
};

struct ListJobsRsp {
  SERDE_STRUCT_FIELD(jobs, std::vector<JobInfo>{});
};

SERDE_SERVICE(MigrationSerde, 0xF1) {
  SERDE_SERVICE_METHOD(start, 1, StartJobReq, StartJobRsp);
  SERDE_SERVICE_METHOD(stop, 2, StopJobReq, StopJobRsp);
  SERDE_SERVICE_METHOD(list, 3, ListJobsReq, ListJobsRsp);
};

}  // namespace hf3fs::migration
