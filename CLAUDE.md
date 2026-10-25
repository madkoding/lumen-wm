# CLAUDE.md

## Repository

- This is a fork: `madkoding/openbox`. Branches, pushes and pull requests always target
  `madkoding/openbox` (base `main`), **never** the upstream Openbox repository.
- Commits and pull request titles are semantic (`feat(shelf): …`, `fix(menu): …`, `docs: …`).

## Checks

- Build: `./bootstrap && ./configure --prefix=$PREFIX && make && make install`.
- Shelf and menus: `tools/shelf-themes.sh $PREFIX` shoots every bundled theme and checks contrast;
  `COMPOSITOR=1` runs picom so the menus take the cairo path. CI runs it
  (`.github/workflows/shelf-themes.yml`).
