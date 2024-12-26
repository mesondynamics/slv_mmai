# snrlab-template-code

Template for all kinds of coding

## Python `venv`

- Create: `$(PYTHON) -m venv --symlinks --system-site-packages --clear $(PYVENV)`
- Upgrade: `$(PYTHON) -m venv --symlinks --system-site-packages --upgrade --upgrade-deps $(PYVENV)`
- Update all packages: `pip --disable-pip-version-check list --outdated --format=json | python -c "import json, sys; print('\n'.join([x['name'] for x in json.load(sys.stdin)]))" | xargs -n1 pip install -U`

## Git

- `git clone --recurse-submodules`
- `git pull --recurse-submodules --all --rebase --prune && git gc --aggressive`
