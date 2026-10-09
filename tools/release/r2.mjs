// Put, get and list objects in the Cloudflare R2 bucket that serves releases.jonbarnes.dev.
//
// WHY THIS EXISTS RATHER THAN THE AWS SDK. This project has no package manager on purpose
// (CLAUDE.md, "Where The Global Rules Do Not Apply": "No package manager. The submodule commit
// SHA is the lockfile"), so pulling in node_modules to upload four files would be the first
// crack in that. R2 speaks S3, S3 signing is AWS Signature V4, and V4 is about sixty lines of
// HMAC against Node's own crypto. Nothing here is imported that Node does not already have.
//
//   node tools/release/r2.mjs put   <key> <file> [content-type] [cache-control]
//   node tools/release/r2.mjs get   <key> [outfile]
//   node tools/release/r2.mjs copy  <fromKey> <toKey>
//   node tools/release/r2.mjs list  [prefix]
//
// CREDENTIALS, in the order they are looked for. The first one found wins, and the script says
// which it used so a release is never signed with a key the person did not expect:
//
//   1. the environment: R2_ACCOUNT_ID, R2_ACCESS_KEY_ID, R2_SECRET_ACCESS_KEY, R2_BUCKET
//   2. ~/.config/r2-personal/credentials.json, the machine store this project SHOULD use
//      (the bare name is work, the -personal suffix is personal; see the global rules)
//   3. the releases hub's own .env.local, which is where they live today
//
// Three is a fallback and not the intended home. See the note this file's caller prints.

import { createHmac, createHash } from 'node:crypto';
import { readFileSync, writeFileSync, existsSync } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';

const REQUIRED = ['R2_ACCOUNT_ID', 'R2_ACCESS_KEY_ID', 'R2_SECRET_ACCESS_KEY', 'R2_BUCKET'];

const parseEnvFile = (text) => {
    const out = {};
    for (const raw of text.split(/\r?\n/)) {
        const line = raw.trim();
        if (!line || line.startsWith('#')) continue;
        const eq = line.indexOf('=');
        if (eq < 1) continue;
        out[line.slice(0, eq).trim()] = line.slice(eq + 1).trim().replace(/^["']|["']$/g, '');
    }
    return out;
};

const loadCredentials = () => {
    if (REQUIRED.every((k) => process.env[k])) {
        return { source: 'the environment', values: Object.fromEntries(REQUIRED.map((k) => [k, process.env[k]])) };
    }

    const store = join(homedir(), '.config', 'r2-personal', 'credentials.json');
    if (existsSync(store)) {
        const values = JSON.parse(readFileSync(store, 'utf8'));
        if (REQUIRED.every((k) => values[k])) {
            return { source: '~/.config/r2-personal/credentials.json', values };
        }
    }

    const hubEnv = join(homedir(), 'Nextcloud', 'development', 'internal', 'releases-hub', '.env.local');
    if (existsSync(hubEnv)) {
        const values = parseEnvFile(readFileSync(hubEnv, 'utf8'));
        if (REQUIRED.every((k) => values[k])) {
            return { source: "the releases hub's .env.local (a fallback, not their home)", values };
        }
    }

    throw new Error(
        'No R2 credentials. Set ' + REQUIRED.join(', ') + ' in the environment, or put them in\n' +
        '~/.config/r2-personal/credentials.json.'
    );
};

const hmac = (key, value) => createHmac('sha256', key).update(value, 'utf8').digest();
const sha256Hex = (value) => createHash('sha256').update(value).digest('hex');

// An S3 request signed with Signature V4. R2 wants region "auto" and the bucket in the path.
const signedRequest = async ({ method, key, body, contentType, cacheControl, query }) => {
    const { source, values } = loadCredentials();
    const account = values.R2_ACCOUNT_ID;
    const bucket = values.R2_BUCKET;

    const host = `${account}.r2.cloudflarestorage.com`;
    // Each path segment is encoded on its own: a slash in the key separates segments and must
    // survive, while a space or a plus in a file name must not.
    const canonicalPath = '/' + [bucket, ...key.split('/')]
        .filter((segment) => segment !== '')
        .map((segment) => encodeURIComponent(segment))
        .join('/');

    const payload = body ?? Buffer.alloc(0);
    const payloadHash = sha256Hex(payload);
    const now = new Date();
    const amzDate = now.toISOString().replace(/[:-]|\.\d{3}/g, '');
    const dateStamp = amzDate.slice(0, 8);

    const headers = {
        host,
        'x-amz-content-sha256': payloadHash,
        'x-amz-date': amzDate,
    };
    if (contentType) headers['content-type'] = contentType;
    // Stored with the object and served back as Cache-Control (the website's pages, 2026-10-08,
    // carry the releases hub's own rules so an upload from here caches exactly as one from there).
    if (cacheControl) headers['cache-control'] = cacheControl;

    const signedHeaderNames = Object.keys(headers).sort();
    const canonicalHeaders = signedHeaderNames.map((h) => `${h}:${headers[h]}\n`).join('');
    const signedHeaders = signedHeaderNames.join(';');

    const canonicalQuery = query
        ? Object.keys(query).sort().map((k) => `${encodeURIComponent(k)}=${encodeURIComponent(query[k])}`).join('&')
        : '';

    const canonicalRequest = [method, canonicalPath, canonicalQuery, canonicalHeaders, signedHeaders, payloadHash].join('\n');
    const scope = `${dateStamp}/auto/s3/aws4_request`;
    const toSign = ['AWS4-HMAC-SHA256', amzDate, scope, sha256Hex(canonicalRequest)].join('\n');

    let signingKey = hmac(`AWS4${values.R2_SECRET_ACCESS_KEY}`, dateStamp);
    for (const part of ['auto', 's3', 'aws4_request']) signingKey = hmac(signingKey, part);
    const signature = createHmac('sha256', signingKey).update(toSign, 'utf8').digest('hex');

    headers.Authorization =
        `AWS4-HMAC-SHA256 Credential=${values.R2_ACCESS_KEY_ID}/${scope}, ` +
        `SignedHeaders=${signedHeaders}, Signature=${signature}`;

    const url = `https://${host}${canonicalPath}${canonicalQuery ? '?' + canonicalQuery : ''}`;
    const response = await fetch(url, {
        method,
        headers,
        body: method === 'PUT' ? payload : undefined,
    });

    return { response, source, bucket };
};

const contentTypeFor = (key) => {
    if (key.endsWith('.yml') || key.endsWith('.yaml')) return 'text/yaml; charset=utf-8';
    if (key.endsWith('.json')) return 'application/json; charset=utf-8';
    if (key.endsWith('.zip')) return 'application/zip';
    if (key.endsWith('.exe')) return 'application/vnd.microsoft.portable-executable';
    if (key.endsWith('.txt') || key.endsWith('.md')) return 'text/plain; charset=utf-8';
    return 'application/octet-stream';
};

const die = (message) => {
    console.error(message);
    process.exit(1);
};

const [, , command, ...rest] = process.argv;

try {
    if (command === 'put') {
        const [key, file, explicitType, cacheControl] = rest;
        if (!key || !file) die('usage: r2.mjs put <key> <file> [content-type] [cache-control]');
        if (!existsSync(file)) die(`no such file: ${file}`);
        const body = readFileSync(file);
        const { response, source, bucket } = await signedRequest({
            method: 'PUT',
            key,
            body,
            contentType: explicitType || contentTypeFor(key),
            cacheControl,
        });
        if (!response.ok) die(`PUT ${key} failed: ${response.status} ${await response.text()}`);
        console.log(`  put  s3://${bucket}/${key}  ${body.length.toLocaleString()} bytes  [credentials: ${source}]`);
    }
    else if (command === 'get') {
        const [key, outFile] = rest;
        if (!key) die('usage: r2.mjs get <key> [outfile]');
        const { response } = await signedRequest({ method: 'GET', key });
        if (response.status === 404) die(`NOTFOUND ${key}`);
        if (!response.ok) die(`GET ${key} failed: ${response.status} ${await response.text()}`);
        const buffer = Buffer.from(await response.arrayBuffer());
        if (outFile) {
            writeFileSync(outFile, buffer);
            console.log(`  got  ${key} -> ${outFile}  ${buffer.length.toLocaleString()} bytes`);
        }
        else {
            process.stdout.write(buffer);
        }
    }
    // Copy by reading and writing back rather than with the S3 copy header: the only things
    // copied here are pointer files of a few hundred bytes, and this needs no second signing path.
    else if (command === 'copy') {
        const [fromKey, toKey] = rest;
        if (!fromKey || !toKey) die('usage: r2.mjs copy <fromKey> <toKey>');
        const got = await signedRequest({ method: 'GET', key: fromKey });
        if (got.response.status === 404) die(`NOTFOUND ${fromKey}`);
        if (!got.response.ok) die(`GET ${fromKey} failed: ${got.response.status}`);
        const body = Buffer.from(await got.response.arrayBuffer());
        const put = await signedRequest({ method: 'PUT', key: toKey, body, contentType: contentTypeFor(toKey) });
        if (!put.response.ok) die(`PUT ${toKey} failed: ${put.response.status} ${await put.response.text()}`);
        console.log(`  copy ${fromKey} -> ${toKey}  ${body.length.toLocaleString()} bytes`);
    }
    else if (command === 'list') {
        const [prefix] = rest;
        const { response } = await signedRequest({
            method: 'GET',
            key: '',
            query: { 'list-type': '2', prefix: prefix || '', 'max-keys': '200' },
        });
        if (!response.ok) die(`LIST failed: ${response.status} ${await response.text()}`);
        const xml = await response.text();
        const keys = [...xml.matchAll(/<Key>([^<]+)<\/Key>/g)].map((m) => m[1]);
        const sizes = [...xml.matchAll(/<Size>(\d+)<\/Size>/g)].map((m) => Number(m[1]));
        if (keys.length === 0) console.log('  (nothing under that prefix)');
        keys.forEach((k, i) => console.log(`  ${String(sizes[i] ?? 0).padStart(12)}  ${k}`));
    }
    else {
        die('usage: r2.mjs put|get|copy|list ...');
    }
}
catch (error) {
    die(String(error.message || error));
}
