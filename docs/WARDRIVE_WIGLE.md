# WiGLE uploads

Uploads share GPS-associated network observations with WiGLE. Recording and
CSV creation do not upload anything. There is no automatic upload on session
completion; individual and bulk uploads require on-device confirmation.

## Setup and upload

1. Stop recording. Under WiFi Upload Settings, choose a trusted network using
   saved profiles, a scan or manual entry. Save a new profile only if wanted.
2. Obtain **API Name** and **API Token** from https://wigle.net/account.
   Enter them under WiGLE API Settings. These are not your account password,
   an encoded Basic-auth string or the local wireless-transfer access code.
3. Select a Saved Session and create its WiGLE CSV. Only observations with
   usable geographic position/time become geographic CSV rows; raw logs stay.
4. Choose Upload Session → Upload to WiGLE and confirm the sharing notice.
   Bulk WiGLE Upload processes eligible pending sessions after its separate
   confirmation/hold. Rejected or ambiguous submissions need review.
5. Check the submission in your WiGLE account. Marking a session as submitted
   is a user assertion after account review, not proof of database processing.

API credentials are held in RAM and cleared on exit. Optional SD provisioning
uses `/wardrive/config/wardrive-setup.json`:

```json
{"wigle":{"api_name":"YOUR_API_NAME","api_token":"YOUR_API_TOKEN"}}
```

Import SD setup requires confirmation and offers deletion afterwards. SD
file deletion is not secure erasure. Legacy `auto_upload` input is not an
instruction to send data automatically; the current UI disables that policy.
Wi-Fi profiles use the existing shared profile store only through explicit
Save. No claim is made that that store has independently verified encryption.

## Time, TLS and failures

Uploads use HTTPS with the platform certificate bundle, hostname/date
verification and redirects disabled. If the platform clock is unset, a
validated GPS clock may be lent to it only for the bounded HTTPS operation;
the original timeline plus elapsed time is restored afterwards. Existing valid
system time is preserved. Wardrive does not start SNTP or bypass certificate
checks. Without valid time the operation fails with WAIT GPS DATE/TIME.
See [clock ownership and concurrency limits](WARDRIVE_LIFECYCLE.md).

The persistent per-session upload state is committed before sending. Submitted
responses retain a transaction ID; pending, failed, confirmed and needs-review
states remain distinct internally. An interrupted or ambiguous submission
that might have sent data is not automatically retried. Check the WiGLE
account before resetting a queue: reset can cause duplicate submissions.
Authentication errors require corrected credentials. Safe retry paths retain
bounded attempts/backoff. Source logs are never deleted after uploading.

Transport failures retain bounded status/error information without printing
credentials, CSV content or server response bodies. Verbose developer traces
are disabled by default. Host upload tests use synthetic offline mocks;
the owner independently reported successful real WiGLE submission.

References: https://api.wigle.net/swagger.json and
https://api.wigle.net/csvFormat.html (CSV v1.6).
