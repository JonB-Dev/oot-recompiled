# gamecontrollerdb.txt

The community game controller mapping database for SDL, shipped so that pads SDL's own tables do
not know are still recognized as game controllers with their buttons in the standard places.

- Source: https://github.com/mdqinc/SDL_GameControllerDB (`gamecontrollerdb.txt`)
- Commit: `5a12daa568d19344f9b6e9286ef5929833b25c7c`, committed 2026-09-11
- Fetched: 2026-09-19, verbatim, 2287 lines, of which 869 are Windows mappings
- License: zlib, in `gamecontrollerdb.LICENSE` beside it, the license the repository carries

The file is data the application reads at startup through SDL's own parser
(`SDL_GameControllerAddMappingsFromFile`); it is never written to and never fetched at run time. To
update it, fetch the file again, record the new commit here, and commit both together.
