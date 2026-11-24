#!/usr/bin/env node
'use strict';
const { FileSearchClient } = require('./index');

const HELP = `FileSearch — Windows / Linux 服务搜索客户端（Node.js >=18）

用法：node clients/node/cli.js [选项] 关键词
      node clients/node/cli.js --status

选项：
  --scope PATH       只搜索指定文件或目录范围
  --limit N          最多返回 N 条，默认 50（1–10000）
  --phase MODE       exact / fuzzy / both，默认 exact
  --ipc-dir PATH     Linux 状态/套接字目录，默认 /var/lib/filesearch
  --endpoint PATH    自定义搜索套接字或 Windows 命名管道
  --timeout MS       连接/响应超时，默认 5000 毫秒
  --json             输出 JSON（64 位身份字段保存为字符串）
  --status           查看服务状态，不搜索
  --help, -h         显示帮助
  --                 后面的参数全部视为关键词

服务必须已启动；Linux 默认需要 sudo 才能访问 root-only 套接字。
例：node clients/node/cli.js --scope "D:\\Develop" --limit 20 FileSearch
    sudo node clients/node/cli.js --scope /home zhongwen
`;

function parseArgs(args) {
    const connection = {}, search = { max_results: 50, phase: 'exact_only' }, words = [];
    let json = false, status = false, help = false;
    const phases = { exact: 'exact_only', fuzzy: 'fuzzy_only', both: 'exact_then_fuzzy' };
    const number = (value, name, max) => {
        if (!/^[0-9]+$/.test(value)) throw new Error(`${name} 必须为正整数`);
        const result = Number(value);
        if (!Number.isSafeInteger(result) || result < 1 || result > max)
            throw new Error(`${name} 必须在 1–${max} 之间`);
        return result;
    };
    for (let i = 0; i < args.length; ++i) {
        const argument = args[i];
        if (argument === '--') { words.push(...args.slice(i + 1)); break; }
        if (argument === '--help' || argument === '-h') { help = true; continue; }
        if (argument === '--json') { json = true; continue; }
        if (argument === '--status') { status = true; continue; }
        if (!argument.startsWith('-')) { words.push(argument); continue; }
        if (!['--scope', '--limit', '--phase', '--ipc-dir', '--endpoint', '--timeout'].includes(argument))
            throw new Error(`未知选项：${argument}`);
        const value = args[++i];
        if (value === undefined || value === '') throw new Error(`${argument} 缺少参数`);
        if (argument === '--scope') search.scope_path = value;
        if (argument === '--limit') search.max_results = number(value, argument, 10000);
        if (argument === '--phase') {
            if (!phases[value]) throw new Error('--phase 必须为 exact、fuzzy 或 both');
            search.phase = phases[value];
        }
        if (argument === '--ipc-dir') connection.ipcDirectory = value;
        if (argument === '--endpoint') connection.searchEndpoint = value;
        if (argument === '--timeout') connection.timeoutMs = number(value, argument, 2147483647);
    }
    const query = words.join(' ');
    if (!help && status && query) throw new Error('--status 不能同时指定关键词');
    if (!help && !status && !query.trim()) throw new Error('请输入搜索关键词；用 --help 查看用法');
    return { connection, search, query, json, status, help };
}

async function run(args, { stdout = process.stdout, stderr = process.stderr,
    clientFactory = options => new FileSearchClient(options), signal } = {}) {
    let options;
    try { options = parseArgs(args); }
    catch (error) { stderr.write(`参数错误：${error.message}\n`); return 2; }
    if (options.help) { stdout.write(HELP); return 0; }
    try {
        const client = clientFactory(options.connection);
        const started = Date.now();
        const response = options.status ? await client.status({ signal })
            : await client.search(options.query, options.search, { signal });
        if (options.json || options.status) {
            stdout.write(JSON.stringify(response, (_, value) => typeof value === 'bigint' ? value.toString() : value, 2) + '\n');
        } else {
            if (!Array.isArray(response.results)) throw new Error('服务返回了无效搜索结果');
            if (response.stale) stderr.write('注意：当前返回缓存结果，后台校验尚未完成，文件可能已发生变化。\n');
            // Keep control characters in filenames from changing terminal state
            // or splitting one result into multiple apparent results.
            for (const entry of response.results)
                stdout.write(String(entry.path).replace(/[\x00-\x1f\x7f-\x9f]/g,
                    character => '\\u' + character.charCodeAt(0).toString(16).padStart(4, '0')) + '\n');
            stderr.write(`${response.results.length} 条结果，耗时 ${Date.now() - started} ms\n`);
        }
        return 0;
    } catch (error) {
        stderr.write(`搜索失败：${error.message}\n`);
        if (['ENOENT', 'ECONNREFUSED', 'EPIPE', 'connection_closed'].includes(error.code))
            stderr.write('请确认 FileSearch 服务已启动，并检查 --ipc-dir / --endpoint。\n');
        if (error.code === 'EACCES') stderr.write('没有连接权限；Linux 默认需要使用 sudo。\n');
        return signal?.aborted ? 130 : 1;
    }
}

if (require.main === module) {
    const cancellation = new AbortController();
    process.once('SIGINT', () => cancellation.abort());
    run(process.argv.slice(2), { signal: cancellation.signal })
        .then(code => { process.exitCode = code; })
        .catch(error => { process.stderr.write(`${error.message}\n`); process.exitCode = 1; });
}
module.exports = { parseArgs, run };
