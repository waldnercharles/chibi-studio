# Held engine Studio test oracles

These files retain Studio test cases from the engine checkout with fixtures migrated to current Chibi asset modules. They are held for case-by-case migration into this repository. No executable target is supplied, and copying them does not establish that the former suites run against the installed SDK.

`PROVENANCE.json` records each current relative path, byte count and SHA256. Each record links to its immutable original manifest entry and retains the original byte count and hash. It includes 78 direct editor-header consumers and seven test helper records, the 180-file engine fixture tree, and 17 existing literal authored asset references. Four of those literal assets are outside the fixture tree. There are 269 unique preserved files. The fixture tree's 52 `.chibi.meta` sidecars were removed on 2026-10-20 when the engine dropped the format; the engine fixtures keep their descriptions as source comments.

The 50 directory, dynamic or missing literal references remain listed in the manifest. This is a preservation inventory, not a complete executable dependency resolution. Some sources include retired private graph executor headers or use the former engine test dispatcher. Those contracts must be replaced with public SDK APIs before an individual case is admitted.

After the accepted external Studio interaction check, 78 editor-dependent engine oracle originals were retired. Seven shared helpers and all engine fixture files retain their existing ownership. Prior snapshots and hashes remain available through the immutable Git links in `PROVENANCE.json`. Historical full UI, GPU and timing suites remain quarantined under the original test policy; no acceptance or performance result is inferred from this copy.
