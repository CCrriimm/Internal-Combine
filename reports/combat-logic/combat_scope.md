# Gameplay appearance analysis scope

The capture examined loaded Python gameplay and rendering functions for character, melee and gun appearance changes in a local BloodStrike process. Network activity was outside the capture scope.

Metadata and bytecode were read through Frida and decoded offline. The original discovery pass did not invoke appearance-changing functions, change character/weapon state, or send server RPCs. It established function signatures and call paths, not account entitlement or universal skin compatibility.
