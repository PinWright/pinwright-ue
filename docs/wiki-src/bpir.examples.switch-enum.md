# bpir.examples.switch-enum

## Switch on Enum

Route execution based on an enum variable's value, with a default fallback.

```
entry event BeginPlay() {
    %sw = switch($WeaponType) [Sword -> @sword, Bow -> @bow, Staff -> @staff, default -> @unknown]

@sword:
    call PrintString(InString: "Melee weapon")
    exec -> @done

@bow:
    call PrintString(InString: "Ranged weapon")
    exec -> @done

@staff:
    call PrintString(InString: "Magic weapon")
    exec -> @done

@unknown:
    call PrintString(InString: "Unknown weapon")

@done:
    call EquipWeapon()
}
```

> **Tests:** Parser: `ParseSwitchEnum` | Compiler: `CompileSwitchEnum` | Decompiler: `SwitchEnumDecompile` | Round-trip: `SwitchEnum`

_See also: call("bpir.examples") for the full index._
