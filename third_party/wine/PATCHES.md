# P5 patches

No patch is applied to a Wine tree by the P5 recipe. The codec implementations
are maintained as normal source in `sdk/win32/codecs/`; they do not masquerade
as unmodified Wine DLLs. Interface metadata in `codecs/` is unmodified.
P4 patches belong to the parallel f5-14B implementation.
