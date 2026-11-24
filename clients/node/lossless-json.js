'use strict';

// Preserve uint64 protocol identities before JSON.parse can round them.
function parse(text) {
    let rewritten = '';
    for (let i = 0; i < text.length;) {
        const start = i;
        if (text[i] === '"') {
            ++i;
            while (i < text.length) {
                if (text[i] === '\\') { i += 2; continue; }
                if (text[i++] === '"') break;
            }
            rewritten += text.slice(start, i);
        } else if (text[i] === '-' || /[0-9]/.test(text[i])) {
            ++i;
            while (i < text.length && /[0-9eE+.\-]/.test(text[i])) ++i;
            const token = text.slice(start, i);
            if (/^-?(0|[1-9][0-9]*)$/.test(token) && !Number.isSafeInteger(Number(token)))
                rewritten += JSON.stringify(token);
            else rewritten += token;
        } else rewritten += text[i++];
    }
    const identities = new Set(['id', 'frn', 'parent_id', 'old_parent_id', 'epoch', 'sequence']);
    return JSON.parse(rewritten, (key, value) => {
        if (identities.has(key)) {
            if ((typeof value !== 'string' && typeof value !== 'number') ||
                (typeof value === 'number' && !Number.isSafeInteger(value)) ||
                !/^(0|[1-9][0-9]*)$/.test(String(value))) throw new SyntaxError('Invalid uint64 protocol identity');
            const integer = BigInt(value);
            if (integer > 18446744073709551615n) throw new SyntaxError('Protocol identity exceeds uint64');
            return integer;
        }
        return value;
    });
}

function stringify(value) {
    const active = new Set();
    function encode(item) {
        if (typeof item === 'bigint') return item.toString();
        if (item === null || typeof item !== 'object') return JSON.stringify(item);
        if (active.has(item)) throw new TypeError('Circular protocol request');
        active.add(item);
        let result;
        if (Array.isArray(item)) result = '[' + Array.from(item, entry => encode(entry) ?? 'null').join(',') + ']';
        else result = '{' + Object.keys(item).flatMap(key => {
            const encoded = encode(item[key]);
            return encoded === undefined ? [] : [JSON.stringify(key) + ':' + encoded];
        }).join(',') + '}';
        active.delete(item);
        return result;
    }
    return encode(value);
}

module.exports = { parse, stringify };
