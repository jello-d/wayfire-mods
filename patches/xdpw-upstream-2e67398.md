# xdpw-upstream-2e67398.patch

Upstream xdg-desktop-portal-wlr commit 2e67398, "screencast: reject
SelectSources for an unknown session", taken verbatim (`git show`), made
after the 0.8.1 we pin. Drop it when the pin moves past it.

## Why

0.8.1's `select_sources` finds the session with a `wl_list_for_each` and
then tests `if (!sess)`, which that loop never leaves NULL, so a
SelectSources for a session that does not exist dereferences the list head
and the backend segfaults. Unreachable from the real frontend (which
creates the session first), and it is how a scratch build crashed under a
wrong test harness on 2026-11-15 (the /var/crash report of that morning).
A backend that can be killed by one malformed call is worth two lines.
