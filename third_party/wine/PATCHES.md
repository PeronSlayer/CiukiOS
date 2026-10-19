# P5 patches

No patch is applied to a Wine tree by the P5 recipe. The codec implementations
are maintained as normal source in `sdk/win32/codecs/`; they do not masquerade
as unmodified Wine DLLs. Interface metadata in `codecs/` is unmodified.
P4 patches belong to the parallel f5-14B implementation.

F5-14B delivery: no Wine source patch is applied and no reused P4 PE is
distributed. In-process COM/RPC transport changes remain unimplemented;
there are no invented or unapplied `.patch` files. DirectSetup is original
CiukiOS source in `sdk/win32/upper/`, built directly rather than patched into
Wine. The complete-source build prerequisite and remaining patches are
recorded in `docs/design/win32-wine-dependency-audit.md` and the B report.
