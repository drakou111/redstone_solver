# Third-party notices

redstone_solver is MIT licensed (see `LICENSE`). The solver program and the mod jar include the
following third-party software; each license text is in `licenses/`.

## Native solver (redstone_solver.exe and its DLLs)

| Component | License | Text |
|---|---|---|
| [OR-Tools](https://github.com/google/or-tools) 9.15 | Apache-2.0 | `licenses/apache-2.0.txt` |
| [Abseil](https://github.com/abseil/abseil-cpp) (from the OR-Tools package) | Apache-2.0 | `licenses/apache-2.0.txt` |
| [SCIP](https://scipopt.org) (from the OR-Tools package) | Apache-2.0 | `licenses/apache-2.0.txt` |
| SCIP's tclique and tinycthread | see text | `licenses/scip-tclique.txt`, `licenses/scip-tinycthread.txt` |
| [SoPlex](https://soplex.zib.de) (from the OR-Tools package) | Apache-2.0 | `licenses/apache-2.0.txt` |
| SoPlex's fmt and zstr | MIT | `licenses/soplex-fmt.txt`, `licenses/soplex-zstr.txt` |
| [HiGHS](https://github.com/ERGO-Code/HiGHS) 1.12 (from the OR-Tools package) | MIT | `licenses/highs.txt` |
| [Protocol Buffers](https://github.com/protocolbuffers/protobuf) (from the OR-Tools package) | BSD-3-Clause | `licenses/protobuf.txt` |
| utf8_range (part of Protocol Buffers) | MIT | `licenses/utf8_range.txt` |
| [RE2](https://github.com/google/re2) (from the OR-Tools package) | BSD-3-Clause | `licenses/re2.txt` |
| [zlib](https://zlib.net) 1.3.1 (from the OR-Tools package) | zlib | `licenses/zlib.txt` |
| [bzip2](https://sourceware.org/bzip2/) (from the OR-Tools package) | bzip2 (BSD-style) | `licenses/bzip2.txt` |
| [CaDiCaL](https://github.com/arminbiere/cadical) 3.0.1 | MIT | `licenses/cadical.txt` |
| [miniz](https://github.com/richgel999/miniz) 3.1.2 | MIT | `licenses/miniz.txt` |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) 0.60.1 | MIT | `licenses/cpp-httplib.txt` |
| [nlohmann/json](https://github.com/nlohmann/json) 3.12.0 | MIT | `licenses/nlohmann-json.txt` |
| [webview](https://github.com/webview/webview) 0.12.0 | MIT | `licenses/webview.txt` |
| Microsoft WebView2 SDK | BSD-3-Clause | `licenses/webview2-sdk.txt` |
| Microsoft Visual C++ runtime (msvcp140, vcruntime140) | Visual Studio redistributable terms | - |

## Web UI (embedded in the solver program)

| Component | License | Text |
|---|---|---|
| [React and React DOM](https://react.dev) | MIT | `licenses/react.txt`, `licenses/react-dom.txt` |
| [three.js](https://threejs.org) | MIT | `licenses/three.txt` |
| [@react-three/fiber](https://github.com/pmndrs/react-three-fiber) | MIT | `licenses/react-three-fiber.txt` |
| [@react-three/drei](https://github.com/pmndrs/drei) | MIT | `licenses/react-three-drei.txt` |
| [zustand](https://github.com/pmndrs/zustand) | MIT | `licenses/zustand.txt` |

Nothing from Minecraft is included: block textures are read at run time from the player's own
game jar.
