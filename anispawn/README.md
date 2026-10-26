# AniSpawn (GitHub CI experiment)

AniSpawn is a separate desktop Electron application with editable **HTML/CSS/JS** interface, not a hosted web app. This isolated project is published on the `anispawn-desktop` branch of Ehynia/KoraLo because the connected GitHub integration cannot create a new repository. It does not modify KoraLo's default branch.

## Scope

- Built-in Jikan metadata catalog and search (requires internet; Jikan does **not** distribute anime video)
- Local bookmark state and theme
- Local file picker and desktop video element
- Linux and Windows installers built by GitHub Actions (if workflow succeeds)
- **Not** the full AniSpawn v0.5 Python + FFmpeg implementation. This minimal CI experiment is deliberately smaller and cannot claim HLS playback, source aggregation or GPU upscaling. Those remain in the local v0.5 project and are not yet published in this branch.

## Development

```sh
npm install
npm start
```

## CI

Workflow is `.github/workflows/anispawn.yml` in this branch. Runs `npm install`, `npm test`, `npm run make:linux` or `npm run make:win` and uploads artifacts. See workflow run status before claiming binaries are available.
