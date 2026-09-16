# bpir.examples.switch-enum-typed

## Switch on Enum (Typed)

Typed enum switch uses `switch_enum<Type>`. See `call("bpir.instructions")` §2.3 for (`switch`, `switch_int`, `switch_string`, `switch_enum<T>`).

```
entry event BeginPlay() {
    %sw = switch_enum<EWeaponType>($WeaponType) [Sword -> @melee, Bow -> @ranged, Staff -> @magic, default -> @none]

@melee:
    call PrintString(InString: "Melee weapon equipped")
    exec -> @done

@ranged:
    call PrintString(InString: "Ranged weapon equipped")
    exec -> @done

@magic:
    call PrintString(InString: "Magic weapon equipped")
    exec -> @done

@none:
    call PrintString(InString: "No weapon")

@done:
}
```

> **Tests:** Parser: `ParseSwitchEnum` | Compiler: `CompileSwitchEnum` | Decompiler: `SwitchEnumDecompile` | Round-trip: `SwitchEnum`

_See also: call("bpir.examples") for the full index._
