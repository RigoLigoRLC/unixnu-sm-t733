# Offline GPU overhead analysis

`analyze-gpu-overhead.py` reads the raw binary sections saved by `gpu-trace.py`.
It uses the same decoding functions as `trace.metadata.jsonl` and Perfetto, so
the raw event arguments remain the input when metadata decoding changes. Run
it with Python 3; binary decoding does not require the Perfetto package:

```sh
python tools/gpu/analyze-gpu-overhead.py CAPTURE_DIRECTORY --output REPORT.json
python tools/gpu/analyze-gpu-overhead.py CAPTURE_72 --baseline CAPTURE_71 --output COMPARISON.json
```

`stage_durations.fixed_550` contains GPU render-pass and copy
durations whose complete timestamp intervals are covered by saved performance
records acknowledging fixed 550 MHz and DDR index 9. `acknowledged_550` also
accepts governor operation at those levels. The `_operation` groups exclude the
first five seconds, and `stage_time_bins` retains five-second distributions for
each configuration. The optional comparison uses those operation groups and matches
stage name, extent, samples, draw count, render mode and available copy fields;
it compares current fixed operation with the baseline's acknowledged operation.
`gpu_stages` retains every interval and the reason for its selection. Stage
durations can overlap and their sum measures accumulated stage time.

`wait_for_idle_by_source` groups emitted CP_WAIT_FOR_IDLE packets by the actual
function, source line, command stream and predicate condition.
`wait_for_idle_by_pass` associates those packets with submitted passes.
`estimated_executions` applies the recorded bin/tile replay count;
`conditional_execution_upper_bound` covers visibility or predicate-dependent
execution. `emitted_packets_without_execution_estimate` preserves command
coverage when replay multiplicity is unavailable. These quantities count
commands; GPU timestamp differences measure elapsed stage time.

`passes` retains actual descriptor flags and GMEM load/store/resolve/clear
operations, resource addresses, encoder IDs and internal pass indexes.
`adjacent_passes_in_same_encoder` records the next pass's split reasons and
copies whose complete GPU intervals lie between the two passes.
`metal_copy_batches` retains copy and dependency-barrier counts, including the
batch's final export barrier. Descriptor load/store flags describe the chosen
pass configuration; operation records describe emitted GMEM operations and
retain their conditional flag.
`attachment_in_gmem_layout` means the pass allocated a GMEM layout slot for
that attachment; `actual_render_mode` records the command buffer's chosen path.
`metal_internal_passes` retains the source's copy and barrier availability flags,
including paths that supply copy counts with an unavailable barrier count.

`system_wait_for_idle_counter.measurements_populated` requires valid adjacent
readings, including a valid zero increment. `cycles` contains event 25's raw
increment across those intervals. `gpu_ddr_samples`, `thermal_limited_records`
and `vote_failures` describe the actual operating points. `losses`,
`metadata_dropped` and `stage_endpoint_coverage` retain incomplete coverage for
each report. Hardware stall cycles and command counts are reported separately
from timestamp durations.
