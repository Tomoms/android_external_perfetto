--
-- Copyright 2023 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.
--

-- Statsd atoms.
--
-- A subset of the slice table containing statsd atom instant events.
CREATE PERFETTO VIEW android_statsd_atoms(
  -- Unique identifier for this slice.
  id INT,
  -- The name of the "most-specific" child table containing this row.
  type STRING,
  -- The timestamp at the start of the slice (in nanoseconds).
  ts INT,
  -- The duration of the slice (in nanoseconds).
  dur INT,
  -- The id of the argument set associated with this slice.
  arg_set_id INT,
  -- The value of the CPU instruction counter at the start of the slice. This column will only be populated if thread instruction collection is enabled with track_event.
  thread_instruction_count INT,
  -- The change in value of the CPU instruction counter between the start and end of the slice. This column will only be populated if thread instruction collection is enabled with track_event.
  thread_instruction_delta INT,
  -- The id of the track this slice is located on.
  track_id INT,
  -- The "category" of the slice. If this slice originated with track_event, this column contains the category emitted. Otherwise, it is likely to be null (with limited exceptions).
  category STRING,
  -- The name of the slice. The name describes what was happening during the slice.
  name STRING,
  -- The depth of the slice in the current stack of slices.
  depth INT,
  -- A unique identifier obtained from the names of all slices in this stack. This is rarely useful and kept around only for legacy reasons.
  stack_id INT,
  -- The stack_id for the parent of this slice. Rarely useful.
  parent_stack_id INT,
  -- The id of the parent (i.e. immediate ancestor) slice for this slice.
  parent_id INT,
  -- The thread timestamp at the start of the slice. This column will only be populated if thread timestamp collection is enabled with track_event.
  thread_ts INT,
  -- The thread time used by this slice. This column will only be populated if thread timestamp collection is enabled with track_event.
  thread_dur INT
) AS
SELECT
  slice.id AS id,
  slice.type AS type,
  slice.ts AS ts,
  slice.dur AS dur,
  slice.arg_set_id AS arg_set_id,
  slice.thread_instruction_count AS thread_instruction_count,
  slice.thread_instruction_delta AS thread_instruction_delta,
  slice.track_id AS track_id,
  slice.category AS category,
  slice.name AS name,
  slice.depth AS depth,
  slice.stack_id AS stack_id,
  slice.parent_stack_id AS parent_stack_id,
  slice.parent_id AS parent_id,
  slice.thread_ts AS thread_ts,
  slice.thread_dur AS thread_dur
FROM slice
JOIN track ON slice.track_id = track.id
WHERE
  track.name = 'Statsd Atoms';


