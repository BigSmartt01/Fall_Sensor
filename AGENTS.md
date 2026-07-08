# Agent instructions — Fall Sensor

## Git submodules

Always keep submodules current with their remote tracking branches.

Tracked submodules (see `.gitmodules`):

| Path | Remote branch |
|------|----------------|
| `firmware_idf/components/DFRobot_BMI160` | `master` |
| `firmware_idf/components/esp-tflite-micro` | `master` |

### Required habits

1. **After clone or when starting firmware work**, initialize and update:
   ```bash
   git submodule update --init --recursive
   git submodule update --remote --merge
   ```
2. **Before commit/push of firmware-related work**, refresh submodules and include any pointer bumps in the same PR/commit when intentional:
   ```bash
   git submodule update --remote --merge
   git status
   ```
3. **Do not leave dirty submodule pointers** uncommitted or unexplained. Either:
   - commit the new SHAs (if you advanced to latest remote), or
   - restore them with `git submodule update --init` if the bump was accidental.
4. **Never ignore** `git status` lines like `modified: … (new commits)` for submodule paths — treat them as real changes.
5. Prefer **remote tip of the configured branch** over random local detached SHAs.

### PowerShell one-liner (Windows)

```powershell
git submodule update --init --recursive
git submodule update --remote --merge
git status
```
