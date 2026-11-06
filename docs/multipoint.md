# Aim points / multipoint

Aim points is a multi-select menu with Head, Neck and Body. It does not use a separate enable toggle. At least one point must remain selected. The selection is saved in the existing reserved options word; no config size or version change is needed. Existing single-bone settings migrate to their matching priority point (spine/pelvis become Body); old Random or enabled Multipoint settings migrate to all three. Hold/toggle, knock filtering and UI preferences retain their original bits.

For each candidate, selected bones are tested in fixed order Head (joint 0), Neck (joint 1), Body (spine joint 2). Missing/invalid positions, range and FOV failures fall through to the next selected bone. With Visible targets only enabled, each point must also pass its own raycast. With visibility filtering off, priority still applies but cover does not cause fallback. The first eligible selected bone is then passed into the existing best-target ranking, prediction and humanization. Retention uses the same path. Multi-point mode does not substitute synthetic head coordinates for missing bones; the single legacy head fallback remains for compatibility.

Visibility now raycasts exactly the requested aim point. Its short-lived cache includes that point's coordinates, so a blocked head cannot cache a blocked result for neck/body. ESP can continue using the most recent entity visibility summary.

Validation: 121 offline tests pass, including per-point priority, missing/occluded/FOV/range fallback, no eligible points, retained targeting, cache separation, mask migration and saved-mask round trips. Release build and embedded payload verification are separate from live game validation.
