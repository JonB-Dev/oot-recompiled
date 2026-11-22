// The application's icon is the medallion (the user, 2026-10-12: "the favicon and app icon should be
// the medalion"; the menus plan, phase 108): the shipped Leaf fan layers (assets/ui/medallion/) with
// the mark of three stacked triangles in its disc, raised to sit optically centered, its gold at
// rest. Each size Windows asks for is rendered at that size by headless Chrome (never one picture
// squeezed) and packed into assets/icon/app.ico. Replaces tools/make_icon.py's mark.
//
//   node tools/app_icon.mjs
//
// Build time only, run by hand when the medallion changes, as make_icon.py was. Needs Chrome or
// Edge and Python with Pillow (to pack the .ico).
import { readFileSync, writeFileSync, mkdirSync, rmSync, existsSync } from "node:fs";
import { execFileSync } from "node:child_process";
import path from "node:path";
import os from "node:os";
import { fileURLToPath, pathToFileURL } from "node:url";
import { medallion } from "../.scaffold/menus/design/splash/source/splash-canvas.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, "..");
const WORK = path.join(os.tmpdir(), "oot-app-icon");
const CHROME = ["C:/Program Files/Google/Chrome/Application/chrome.exe",
  "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"].find(existsSync);
const SIZES = [16, 20, 24, 32, 40, 48, 64, 256];

// The medallion at rest, as the splash draws it: leaves still, no glint (it shows only while it
// runs round the rim), no ground shadow (an icon stands alone). Square about the disc's center
// (100, 90), wide enough for the leaves.
const inner = s => s.replace(/^\s*<svg[^>]*>/, "").replace(/<\/svg>\s*$/, "");
const art = inner(medallion("x").replace(/<g class="gl">[\s\S]*?<\/g><\/g>/, ""))
  .replace(/\s(?:class|style|aria-hidden)="[^"]*"/g, "")
  .replace(/<ellipse cx="100" cy="168"[^>]*\/>/, "");
const R = 87;   // half the square: the leaves' reach from the disc's center, and a little air
const svg = size => `<svg xmlns="http://www.w3.org/2000/svg" width="${size}" height="${size}" viewBox="${100 - R} ${90 - R} ${2 * R} ${2 * R}">${art}</svg>`;

rmSync(WORK, { recursive: true, force: true });
mkdirSync(WORK, { recursive: true });
const pngs = SIZES.map(size => {
  const page = path.join(WORK, `${size}.html`);
  const out = path.join(WORK, `${size}.png`);
  writeFileSync(page, `<!doctype html><html><head><style>html,body{margin:0;background:transparent;overflow:hidden}svg{display:block}</style></head><body>${svg(size)}</body></html>`);
  execFileSync(CHROME, ["--headless=new", "--disable-gpu", "--hide-scrollbars", "--no-first-run",
    `--user-data-dir=${path.join(WORK, `${size}-profile`)}`, "--default-background-color=00000000",
    "--force-device-scale-factor=1", `--window-size=${Math.max(size, 64)},${Math.max(size, 64)}`, `--screenshot=${out}`,
    pathToFileURL(page).href], { stdio: "ignore", timeout: 60000 });
  return [out, size];
});
const target = path.join(ROOT, "assets", "icon", "app.ico");
const py = [
  "import sys, json",
  "from PIL import Image",
  "items = json.loads(sys.argv[1])",
  "ims = [Image.open(p).convert('RGBA').crop((0, 0, s, s)) for p, s in items]",
  "big = max(ims, key=lambda i: i.size[0])",
  "big.save(sys.argv[2], format='ICO', sizes=[i.size for i in ims], append_images=[i for i in ims if i is not big])",
].join(String.fromCharCode(10));
execFileSync("python", ["-c", py, JSON.stringify(pngs), target]);
rmSync(WORK, { recursive: true, force: true });
console.log(`APP_ICON_OK ${SIZES.length} sizes to assets/icon/app.ico`);
