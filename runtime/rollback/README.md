# Rollback (MVC-style layout)

Online rollback for the recompiled game: each tick the game runs a frame with the remote player's pad GUESSED; when the real
pad arrives and differs, the game state is put back to before that frame and the frames run again.

| Layer | Folder | Job | May call |
|---|---|---|---|
| Controller | `controller/` | The per-tick loop: read pads, run a frame, notice a wrong guess, order a load, run the frames again | model, services |
| Model | `model/` | The data: pad history (`input_history.c`), the saved game state (`game_state.c`), the ring of the last 8 states (`snapshot_ring.c`), which pages the game writes (`page_tracker_learned.c`) | nothing above it |
| Services | `services/` | Things the controller uses that know nothing about memory: wire format, clock sync, the network session and match handshake, the ranked ladder's match result (`ladder_report.c`) | nothing above it |

Public contract of the game-free parts: `rollback.h`. What `page_tracker_learned.c` shares with `game_state.c` / `snapshot_ring.c`:
`model/game_state_internal.h`.

Where each file used to be: `rb.h` -> `rollback.h`, `rb_input.c` -> `model/input_history.c`, `rb_learned.c` -> `model/page_tracker_learned.c`,
`r1_ring.c` -> `model/snapshot_ring.c`, `r1_state.c` -> `model/game_state.c`, `r1_internal.h` -> `model/game_state_internal.h`,
`rb_rollback.c` -> `controller/rollback_controller.c`, `rb_net.c` -> `services/net_session.c`, `rb_wire.c` -> `services/wire_format.c`,
`rb_timesync.c` -> `services/clock_sync.c`, `rb_hs.h` -> `services/handshake.h`.

## The files of the model (split out of the old r1_state.c, r1_ring.c; each is its own translation unit)

| File | What it is |
|---|---|
| `model/game_state.c` | Entry points the rest of the exe calls (`r1_init`, `r1_poll`, `r1_pad`, frame end, statistics) |
| `model/memory_map.c` | Which ranges of the game's memory the save covers |
| `model/threads.c` | The other guest threads parked while memory is copied; nn::os objects; the job system's fibers |
| `model/full_save.c` | The full save / restore (the base the ring starts from) |
| `model/snapshot_ring.c` | The ring of the last 8 states (capture, load, rebase) |
| `model/page_tracker_learned.c` | Which pages the game writes (the learned set) and the presentation pool (Windows only) |
| `model/page_tracker_posix.c` | The same job on Linux: window 1 write watch, window 2 page compare, extras write watch, parallel page jobs |
| `rb_park.c` | The Linux cooperative park gate: guest threads stop at SDK import boundaries while memory is copied |
| `model/runahead.c` | Run-ahead of one frame (SSBU_RUNAHEAD) on the same save / restore |
| `model/guest_heap.c` | The guest C heap inside the snapshot (SSBU_R1) |
| `model/tracking_experiments/` | Older experiments kept because `tests/r1_*.py` use them: fast save, store marks, write protection |
| `debug/ring_simulation.c` | Test and measurement code of the ring: forced-rollback simulation, profilers, verifier |

`model/game_state_internal.h` is the list of what these files share (it is generated from what clang says each file uses of the
others; a name appears there only because another file calls it). Shrinking that list is the next job: replace shared globals by
structs and accessor functions, and introduce the `PageTracker` interface and a `state.h` facade for the controller.
