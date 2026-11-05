# Pinned Ibex fixtures

These are the unmodified `ibex_branch_predict` and `ibex_controller` module
bodies extracted from the sv2v-converted Ibex bundle supplied to this project.
Ibex upstream: <https://github.com/lowRISC/ibex>, related SystemVerilog sources:
`rtl/ibex_branch_predict.sv` and `rtl/ibex_controller.sv`.

The supplied converted bundle did not record its upstream Git revision or
sv2v version. These fixtures are identified by their exact SHA-256 hashes in
`source.json`; they are not claimed to match current upstream HEAD or a named
release. That file also records the original bundle's hash. The conversion
contains `_sv2v_0` and flattened package constants; those are preserved.

The modules are checked in so normal tests need neither network access,
sv2v, nor a local workflow directory. From the repository root:

```sh
python3 tests/cellfuzz/fixtures/ibex/extract.py
# Reproduce from a supplied bundle with identical module bodies:
python3 tests/cellfuzz/fixtures/ibex/extract.py /path/to/converted-ibex.v --output /tmp/ibex-fixtures
make -C tests cellfuzz/Makefile
make -C tests/cellfuzz value_roundtrip.sh
```

`extract.py` checks the module hashes before writing. It rejects changed or
duplicate module bodies, so replacing the upstream source cannot silently
change the regression. Portability uses the checked-in fixtures, not an
unrecorded upstream regeneration recipe. Ibex is licensed under Apache-2.0;
see `LICENSE` and `NOTICE` alongside the fixture.
