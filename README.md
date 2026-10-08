EADME.md

```markdown
# gmnisrv

**Notice**: this project is no longer maintained. It is provided as-is.
See the "Security" section below before deploying.

gmnisrv is a simple [Gemini](https://gemini.circumlunar.space) protocol server.

## Security

This code has known issues and has not been audited. Before exposing it to
the public internet:

- Verify the vendored `src/regexp.c` / `src/libunicode.c` from QuickJS match
  a known-good upstream commit.
- Audit the CGI path in `src/serve.c` (`serve_cgi`). It forks and execs
  scripts with limited sandboxing. Run it only as a dedicated unprivileged
  user, ideally inside a container or jail.
- Symlink resolution in `serve_request` follows links up to three hops but
  does not verify the resolved path stays within `route->root`. Use
  `realpath()`-based prefix checks if this matters for your deployment.
- Client certificate verification is currently TOFU-style (any certificate
  is accepted). Do not rely on it for authentication without additional
  checks.

## Installation

Dependencies:

- A C11 compiler and a POSIX-like system
- OpenSSL
- Mailcap (for /etc/mime.types)
- [scdoc](https://git.sr.ht/~sircmpwn/scdoc) (optional)

You must also vendor the QuickJS libregexp sources:

```

curl -L -o src/regexp.c    https://raw.githubusercontent.com/bellard/quickjs/master/libregexp.c
curl -L -o src/libunicode.c https://raw.githubusercontent.com/bellard/quickjs/master/libunicode.c

```

Then adjust the includes at the top of those files as needed (they include
`cutils.h`, `libunicode.h`, `libregexp.h`, `libregexp-opcode.h` — some of
which may already exist in `include/`).

```

$ mkdir build
$ cd build
$ ../configure --prefix=/usr
$ make

make install

```

## Configuration

By default it reads from `/etc/gmnisrv.ini`, but this can be changed by passing
the `-C` flag to the `gmnisrv` daemon. A sample config.ini is provided at
`/usr/share/gmnisrv/gmnisrv.ini`:

```

Space-separated list of hosts

listen=0.0.0.0:1965 [::]:1965

[:tls]

Path to store certificates on disk

store=/var/lib/gemini/certs

Optional details for new certificates

organization=gmnisrv user

[example.org]
root=/srv/gemini/example.org

[example.com]
root=/srv/gemini/example.com

```

For full details on gmnisrv configuration, consult the **gmnisrv**(5) manual
page.

## Usage

See **gmnisrv**(1).
```

---
