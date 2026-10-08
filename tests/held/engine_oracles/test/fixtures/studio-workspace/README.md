# Studio workspace contracts

This synthetic project owns two scene subjects, four UI documents and an idle
replay. It exercises designer panels, camera overlays, assets virtualization,
graph completion and graph tab lifetimes without a playable game composition.

Movement and Weapon, their callable dependencies and optional metadata are frozen
regression snapshots. Weapon node48 and its layout must stay stable for completion
and close/reopen checks. Components.kdl supplies the reflected labels those checks
use. `fixtures.json` records extraction sources, purposes and hashes.

The Firefight name belongs to the existing replay assertion; its sixty-frame idle
scenario only exercises authoring lifetimes. Game encounter/input acceptance and
expensive thumbnail/benchmark cases retain their original owning projects.
No Studio or GPU test admission is implied by this fixture migration.
