Unmodified files from zlib 1.3.1 (https://zlib.net/zlib-1.3.1.tar.gz, sha256
9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23), under the zlib license in LICENSE.

Only inflate_fast (inffast.c) and adler32_z (adler32.c) are built, through ../NativeZlib.c, which runs them natively
in place of a game's emulated copies (see ../NativeRedirect.cpp).
