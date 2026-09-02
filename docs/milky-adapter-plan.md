# Milky Adapter Follow-up Plan

The completed implementation is limited to the server adapter, configuration,
authenticated WebHook ingress, API mappings, identity routing and CQ/SealDice
media conversion. No incomplete Web UI or mock protocol implementation is kept
in the source tree.

1. Build on a machine with the project CMake/vcpkg environment and run the
   server test suite. Add focused unit tests for CQ and SealDice image, record,
   video, file, mention and reply conversion.
2. Run a Milky implementation locally. Verify `get_login_info`, group/friend
   list cache refresh, group member roles, group files, file download URLs,
   group file upload and merged-forward delivery.
3. Configure a real public WebHook endpoint and replay every Milky event type.
   Confirm request approval, group invitation policy, recall, poke, group file
   upload and self-echo filtering work with production-shaped payloads.
4. Update the separately maintained Web UI adapter form to expose `milky`,
   endpoint, API token, WebHook base URL and WebHook token. Display the derived
   callback URL without exposing the token after it has been saved.
5. Add an integration fixture that serves schema-conformant Milky API responses
   and WebHook events, then run it in CI with the adapter tests.
