# Refactoring fixtures

`references` owns the asset relationships exercised by `test/refactor.c`:
Pursuit has four attachments, MenuButton has three widget uses, and Basalt,
BasaltCave, TrailEmitter and FreeMotion each have one direct reference. The
Damage definition links its payload to HullDamage and ProjectileHit. Weapon,
HullDamage and SwarmerEffects exercise linked WeaponActions/DirectionMath
functions and rename behavior without removing native compiler checks.

Selected executable graphs, event definitions and UI documents were captured
from the former Arena2D sample; these copies are regression inputs owned by the
refactoring tests. Prototypes and the scene contain only the relationships the
tests exercise. Material and field graphs provide a valid constant surface and
field without retaining game rendering content.

`values` retains ArenaDropBehavior's authored struct-field use of ArenaExperience,
a prototype reference to the same material, and an OrbitFlipper action binding.
Studio navigation key fixtures continue to come from product Studio assets.

These fixtures do not load external game repositories or launch scenes.
