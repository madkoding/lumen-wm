# CLAUDE.md

## Repository

- This is a fork: `madkoding/openbox`. Branches, pushes and pull requests always target
  `madkoding/openbox` (base `main`), **never** the upstream Openbox repository.
- The project is **Lumen Window Manager** ("Lumen" for short), based on Openbox. It installs as `lumen-wm` (package
  and binary) with an `openbox` symlink, and reads `~/.config/lumen` before `~/.config/openbox`. Source dirs,
  theme dirs (`openbox-3`), XML root nodes and X properties keep the Openbox names.
- Commits and pull request titles are semantic (`feat(shelf): …`, `fix(menu): …`, `docs: …`).

## Checks

- Build: `./bootstrap && ./configure --prefix=$PREFIX && make && make install`.
- Window manager behaviour: `make tests/contract/contract-x11 && tests/contract/run.sh $PREFIX`
  runs the contract cases (`tests/contract/`) on Xvfb. Every refactor towards the Wayland backend keeps
  them green; a new behaviour gets a case first.
- Shelf and menus: `tools/shelf-themes.sh $PREFIX` shoots every bundled theme and checks contrast;
  `COMPOSITOR=1` runs picom so the menus take the cairo path. CI runs it
  (`.github/workflows/shelf-themes.yml`).
