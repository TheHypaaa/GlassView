# Code signing GlassView Native

Windows SmartScreen warns about any unsigned .exe downloaded from the internet
("Windows protected your PC"). The warning goes away once the exe is signed with a
trusted code-signing certificate (and, for new certificates, after it has built up reputation).

## Getting a certificate

| Option | Cost (2026, approx.) | Notes |
|---|---|---|
| **Azure Artifact Signing** (formerly Trusted Signing) | ~10 USD / month | Microsoft-run, cheapest for individuals in the EU/US; identity check required. Signs from the cloud with `signtool` + the Trusted Signing dlib. |
| **Certum Open Source Code Signing** | ~25–60 EUR / year | For open-source projects; certificate on a smart card / cloud HSM. |
| **SignPath Foundation** | free for OSS | Signs releases of qualifying open-source projects from CI. |
| Commercial OV certificate (Sectigo, DigiCert …) | 200–500 EUR / year | Must live on a hardware token since 2023. |

A self-signed certificate does **not** remove the SmartScreen warning.

## Signing

On Windows with the Windows SDK:

```
signtool sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 /a GlassViewNative.exe
```

(Azure Artifact Signing: add `/dlib Azure.CodeSigning.Dlib.dll /dmdf metadata.json`.)

From the Linux build machine with a .pfx file:

```
GV_SIGN_PFX=cert.pfx GV_SIGN_PASS=secret ./build.sh windows
```

(needs `osslsigncode`; hardware-token certificates must be signed on Windows with signtool.)

## Updates

GlassView checks `https://api.github.com/repos/<owner>/<repo>/releases/latest` (Settings › Updates).
Publish a GitHub release with a tag like `v2.6.0` and attach the zip; users get a
"Download v2.6.0…" button. Bump `src/version.hpp` before building a release.
