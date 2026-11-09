SQLite 3.54.1, public domain, from the official amalgamation download.
The CMake file pins the archive by SHA-256. Sources are cached in `.deps/`
or fetched into the build directory. No writable SQLite database is opened
on the device: the AFC importer supplies its own read-only snapshot VFS.

Source and license: https://www.sqlite.org/copyright.html
