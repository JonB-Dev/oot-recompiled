/**
 * Send the showcase captures to a person who is not at this machine.
 *
 *   node tools/harness/mail-shots.mjs
 *   node tools/harness/mail-shots.mjs --to somebody@example.com
 *
 * ONE PAIR PER MESSAGE (2026-09-27: "limit it to a pair an email"), and the full PNGs rather than
 * anything squeezed: a pair is about four megabytes, the provider allows forty, and the whole
 * point of these is to look closely at lighting. Squashing them to JPEG to save room nobody needed
 * would be throwing away the thing being judged.
 *
 * The "on" and "off" of a pair go in the same message deliberately. They are the same frame with
 * one line of configuration changed, so seen together they are an argument and seen apart they are
 * two screenshots.
 */
import { readFileSync, readdirSync } from "node:fs";
import { homedir } from "node:os";
import { join } from "node:path";

const read = (p) => JSON.parse(readFileSync(p, "utf8").replace(/^﻿/, ""));
const rs = read(join(homedir(), ".config", "resend-personal", "credentials.json"));

const TO = process.argv.includes("--to")
  ? process.argv[process.argv.indexOf("--to") + 1]
  : "hello@jonbarnes.dev";
const FROM = '"OoT: Recompiled | Jonathan Barnes" <hello@jonbarnes.dev>';
const SHOTS = join(
  "C:",
  "Users", "jon", "Nextcloud", "development", "internal", "games",
  "Legend of Zelda, The - Ocarina of Time (USA)",
  "build-cmake", "harness", "showcase",
);

/** What each set is, in the words that say why it was chosen. */
const WHY = {
  "navi-field-night": "an empty field at night, where the only light is what Link carries",
  "kakariko-night": "the village after dark, lanterns and the windmill on the hill",
  "graveyard-night": "almost no light at all: the clearest of the set",
  "goron-city": "inside the mountain (note: this one landed in the entry corridor rather than the torch-lit cavern)",
  "temple-of-time": "daylight through the windows onto stone",
  "kakariko-morning": "the village early",
  "kakariko-noon": "the village at noon",
  "kakariko-sunset": "the village at sunset, long shadows down the hill",
  "windmill-noon": "inside the windmill",
  "lake-hylia-sunset": "the sun low across open water",
};

const files = readdirSync(SHOTS).filter((f) => f.endsWith(".png") && f !== "windmill-noon.png");

// Group into sets: a pair where there is one, otherwise the single on its own.
const sets = new Map();
for (const file of files) {
  const base = file.replace(/-(on|off)\.png$/, "").replace(/\.png$/, "");
  if (!sets.has(base)) sets.set(base, []);
  sets.get(base).push(file);
}

const attach = (file) => ({
  filename: file,
  content: readFileSync(join(SHOTS, file)).toString("base64"),
});

const send = async (subject, html, files) => {
  const res = await fetch("https://api.resend.com/emails", {
    method: "POST",
    headers: { Authorization: `Bearer ${rs.RESEND_API_KEY}`, "Content-Type": "application/json" },
    body: JSON.stringify({ from: FROM, to: TO, subject, html, attachments: files.map(attach) }),
  });
  const body = await res.json();
  return { ok: res.ok, id: body.id, error: body.message ?? body.error };
};

const shell = (title, note, lines) => `
<div style="font-family:system-ui,-apple-system,sans-serif;background:#1b130d;color:#f2e7d5;padding:28px;">
  <div style="color:#c9a227;font-size:11px;letter-spacing:0.18em;text-transform:uppercase;">Ray traced lighting</div>
  <div style="font-size:21px;padding-top:8px;">${title}</div>
  <p style="color:#c0a884;font-size:15px;line-height:1.6;max-width:56ch;">${note}</p>
  <ul style="color:#c0a884;font-size:14px;line-height:1.7;">${lines.map((l) => `<li>${l}</li>`).join("")}</ul>
  <p style="color:#9c8a70;font-size:12px;">1920 by 1080. Same frame, same clock, same position; the only
  difference between an on and an off is the one line that turns the lighting on.</p>
</div>`;

let sent = 0;
const failed = [];

for (const [name, group] of sets) {
  const pair = group.length === 2;
  const on = group.find((f) => f.endsWith("-on.png"));
  const off = group.find((f) => f.endsWith("-off.png"));
  const ordered = pair ? [on, off] : group;

  const subject = pair
    ? `Ray tracing on and off: ${name.replace(/-/g, " ")}`
    : `Ray tracing: ${name.replace(/-/g, " ")}`;

  const html = shell(
    name.replace(/-/g, " "),
    WHY[name] ?? "",
    pair
      ? [`<b>${on}</b> is with the lighting on, at the recommended settings`,
         `<b>${off}</b> is the same frame with it off`]
      : [`<b>${ordered[0]}</b>, with the lighting on`],
  );

  const result = await send(subject, html, ordered);
  if (result.ok) {
    console.log(`  sent  ${name} (${ordered.length} attachment${ordered.length > 1 ? "s" : ""})`);
    sent += 1;
  } else {
    console.log(`  FAILED ${name}: ${result.error}`);
    failed.push(name);
  }
  // A moment between messages: the provider rate limits, and a burst of ten is exactly the shape
  // it limits.
  await new Promise((r) => setTimeout(r, 1200));
}

console.log(`\n${sent} message(s) to ${TO}`);
if (failed.length) console.log(`did not send: ${failed.join(", ")}`);
