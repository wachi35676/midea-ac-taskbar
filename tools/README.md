# Python tools

Needs `pip install msmart-ng`. They read `ac_creds.json` from the project root (create it with `get_key.py`).

| Script | Purpose |
|---|---|
| `get_key.py` | One-time: fetch the token/key with your Midea app login (`--ip`, `--region` optional). |
| `query.py`, `probe.py` | Read everything the unit reports; dump raw capability data. |
| `write_test.py`, `running_test.py`, `eco_levels_test.py`, `side_effects.py`, `long_run.py` | Experiments that change settings on the real unit and restore them. **They switch your AC on and off and change settings; run them only when that is fine.** |
| `gen_vectors.py` | Generate the golden vectors (`native/tests/vectors.txt`) from msmart-ng and live frames. |
| `test_native.py`, `test_backend.py` | Cross-check the C++ library and the Python backend against the unit. |
