# Dummy headers

Stubs so the decomp's headers compile freestanding, without having built the decomp.

Empty on purpose. A header lands here only when a patch includes a decomp header that needs a
definition the patch build has not produced, which is usually an asset header. Adding them
speculatively would be inventing declarations for things nothing references.
