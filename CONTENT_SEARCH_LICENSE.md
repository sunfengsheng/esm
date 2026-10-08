# Everything SM Content Search licensing

The standalone Everything SM Content Search application is distributed under
the **GNU General Public License, version 2 or (at your option) any later
version** (`GPL-2.0-or-later`). The complete license text is in
`LICENSES/GPL-2.0-or-later.txt`.

This GPL grant covers the project-owned source and build material needed to
build the content-search binaries, including:

- `src/content/**`;
- `src/apps/content_service/**`, `src/apps/content_gui/**`, and
  `src/apps/content_cli/**`;
- `src/platform/windows/content_named_pipe.cpp`,
  `src/platform/windows/content_roots.cpp`, and
  `src/platform/windows/directory_watcher.cpp`;
- `include/esm/content_*.hpp` and `include/esm/directory_watcher.hpp`;
- `cmake/BuildXapian.cmake`, `cmake/build-xapian-mingw.sh`, the content-search
  portions of `CMakeLists.txt`, and the content packaging scripts;
- `tests/content_test.cpp` and `tools/generate_content_icon.py`;
- content-search icons and resources under `assets/icon/content_search*`, plus
  `cmake/version.rc.in` as used by the content executables.

The files above are copyright the everything_sm contributors and may be
redistributed and/or modified under GPL-2.0-or-later. They are provided without
warranty, to the extent permitted by applicable law.

The bundled Xapian Core source in `third_party/xapian-core` is upstream work
distributed under GPL-2.0-or-later by its respective copyright holders. Its
upstream license is also included as `third_party/xapian-core/COPYING` and is
installed as `XAPIAN-COPYING`.

The content-search installer is kept separate from the file-name search
installer. This license grant does not change the distribution terms of
unrelated file-name-search-only components unless a file explicitly states
otherwise.

Each public content-search binary release is accompanied on the same GitHub
Release page by a version-matched `everything-sm-content-<version>-source.zip`
archive and SHA-256 file. That archive includes the bundled Xapian source and
the build scripts needed for the released binaries.
