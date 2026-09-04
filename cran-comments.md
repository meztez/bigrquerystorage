## R CMD check results

Duration: 1m 13.8s

❯ checking installed package size ... NOTE
  installed size is 17.2Mb
  sub-directories of 1Mb or more:
    libs  16.8Mb
      
❯ Files which contain pragma(s) suppressing diagnostics:
  'src/google/api/annotations.pb.h' 'src/google/api/client.pb.h'
  'src/google/api/field_behavior.pb.h' 'src/google/api/http.pb.h'
  'src/google/api/launch_stage.pb.h' 'src/google/api/resource.pb.h'
  'src/google/cloud/bigquery/storage/v1/arrow.pb.h'
  'src/google/cloud/bigquery/storage/v1/avro.pb.h'
  'src/google/cloud/bigquery/storage/v1/protobuf.pb.h'
  'src/google/cloud/bigquery/storage/v1/storage.pb.h'
  'src/google/cloud/bigquery/storage/v1/stream.pb.h'
  'src/google/cloud/bigquery/storage/v1/table.pb.h'
  'src/google/rpc/status.pb.h'

0 errors ✔ | 0 warnings ✔ | 2note ✖

Tests :  [ FAIL 0 | WARN 0 | SKIP 0 | PASS 32 ]
Package checks : https://github.com/meztez/bigrquerystorage/actions/runs/12659190537/job/35277760840
