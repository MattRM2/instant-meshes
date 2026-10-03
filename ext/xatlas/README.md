xatlas, vendored for --uv unwrap.

Source:   https://github.com/jpcy/xatlas
Commit:   f700c7790aaa030e794b52ba7791a05c085faf0c (2022-07-25, master)
Files:    source/xatlas/xatlas.cpp, source/xatlas/xatlas.h, LICENSE (MIT), unmodified
SHA-256:  xatlas.cpp  0ED0283AAD005C94738CB0CC4612DBA264379D29DEA5B3C9B242F2D4752D5DF4
          xatlas.h    E7675335AD8AB1C1CC9060AD153CF6B8BA2EE914282044EB5F02C49590218FBD
          LICENSE     2C16D5B1C2808277FE975B4F70F0FD9AFC9B3BCF04E6C676665A82CB2D5579E3
Reviewed: standard C++ library only (threads), no network, no process, no
          registry; file output only in debug exports disabled at compile
          time (XA_DEBUG_EXPORT_* = 0).
