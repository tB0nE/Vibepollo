# CI in the Meteor-Vibepollo fork

## Pages is opt-in

The upstream `Nonary/Vibepollo` site keeps its automatic deployment. Forks skip
that job unless the repository Actions variable `ENABLE_GITHUB_PAGES` is exactly
`true`. Before opting in, enable Pages in the repository settings and choose
GitHub Actions as its build source. No existing build/test checks are disabled.

## Arch PR source

GitHub checks out a synthetic merge commit for a pull request. That commit may
not be reachable from a normal clone of the repository. Arch PR builds now seed
makepkg's bare mirror from the verified controller checkout, just like explicit
validation runs. They use `--holdver` and check the expected tree. The helper
continues to reject a dirty checkout, wrong commit or wrong origin. Regular push
packaging and the restrictions on validation caches/signing remain unchanged.

The regression fixture creates an actual detached, branch-unreachable merge
commit and exercises the workflow's source-preparation block and real helper.
Run `/usr/bin/python3 -m unittest discover -s .github/scripts -p 'test_*.py'`.

## Windows producer prerequisite remains enforced

The full Windows job currently stops correctly before compilation:

- Consumer source: `third-party/libvirtualdisplay` at
  `908ad7380b22e5aff66f830417c4e040f57ef52e` in `tB0nE/libvirtualdisplay`.
- Reviewed prebuilt lock: `third-party/libvirtualdisplay-release/windows-x64.json`,
  upstream v1.6.5 at `b6874684d82925d0d76f487d8e4ba3ec71c9721a`.
- The source difference includes intentional Interactive Users access fixes in
  the driver/broker and static MinGW broker linkage. Do not downgrade the source
  or accept the old binary as if it contained those fixes.

To unblock full Windows CI:

1. Build and publish a producer package from the exact consumer source revision,
   using the driver's Windows release pipeline (WDK and a consistent release
   signing identity). The fork currently has no published matching release or
   configured release signing credentials. Do not commit signing material.
2. Verify the tag/source, archive/evidence hashes, driver version, certificate
   identity and per-file inventory. Update the consumer lock and repository/tag
   declarations together, plus the exact-identity fixtures in
   `scripts/tests/test_display_producer_provenance.ps1`,
   `tests/unit/test_virtual_display_packaging_release_lock.py`, the typed C++
   packaging expectations and any producer follow-up fixtures.
3. Rerun mocked provenance tests (including cache tampering and moved-tag cases),
   then full Windows CI. Its source equality, digests, inventory and signature
   checks must remain enabled.

The producer release workflow expects repository secrets
`VDD_SIGNING_CERT_PFX_BASE64` and `VDD_SIGNING_CERT_PASSWORD`. Configure these via
GitHub settings or a secure local process; never paste their values into chat.
This is a Windows producer/signing task, not a reason to suppress host CI or
change the already published Nightfall preview artifacts.
