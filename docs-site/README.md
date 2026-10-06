# Cortrix documentation site

Source for the Cortrix documentation site at <https://cortrix.ai/docs/>, built
with the [OINK](https://oink.pgsty.com/) Hugo theme.

## Requirements

- Hugo Extended 0.160.1 or newer (the `Docs` workflow uses 0.167.0)
- Go 1.27 or newer
- Git
- Python 3 with PyYAML

## Preview

The API reference renders a filtered copy of the OpenAPI spec. Generate it
first, and again whenever `api/` changes. It needs Python 3 with PyYAML.

```bash
python3 docs-site/scripts/filter_openapi.py api docs-site/.openapi
cd docs-site
hugo server
```

Open `http://localhost:1313/docs/`.

## Strict build

```bash
cd docs-site
hugo --cleanDestinationDir --gc --minify --environment production \
  --printPathWarnings --panicOnWarning
```

## Layout

```text
docs-site/
├── hugo.yaml          site configuration
├── go.mod             pins the OINK theme module
├── scripts/           filter_openapi.py, which writes the spec copy the site renders
├── data/home/         landing page content
├── assets/            logo and theme styles
└── content/docs/      documentation pages
```

Pages in `content/docs/` are published without the `docs` section in their URL:
`content/docs/get-started/quickstart.md` is served at
`/docs/get-started/quickstart/`. Links between pages are written as
`/docs/<section>/<page>/`.

The `Docs` workflow runs the strict build on every pull request that changes
`docs-site/` or `api/`, and publishes the site when a change reaches
`release/1.0`.
