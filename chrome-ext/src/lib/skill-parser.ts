import type { X402Service, X402Category, ServiceEndpoint, ServiceParam } from './x402-services';

export interface ParsedSkill {
  name: string;
  description: string;
  method: 'GET' | 'POST';
  fullUrl: string;
  baseUrl: string;
  path: string;
  price: string;
  params: ServiceParam[];
  category: X402Category;
  skillUrl?: string;
  docsUrl?: string;
}

const LINE_PATTERNS: Record<string, RegExp> = {
  skill: /^Skill:\s*(.+)/i,
  docs: /^Docs:\s*(.+)/i,
  endpoint: /^Endpoint:\s*(GET|POST|PUT|PATCH|DELETE)\s+(.+)/i,
  price: /^Price:\s*(.+)/i,
  model: /^Model:\s*(.+)/i,
  name: /^Name:\s*(.+)/i,
  category: /^Category:\s*(.+)/i,
  params: /^Params?:\s*(.+)/i,
};

const CATEGORY_KEYWORDS: Record<X402Category, string[]> = {
  ai: ['image', 'video', 'llm', 'generation', 'inference', 'model', 'ai', 'ml', 'gpu', 'sora', 'dall-e', 'flux', 'nano'],
  'crypto-data': ['token', 'price', 'chart', 'market cap', 'crypto data'],
  defi: ['defi', 'swap', 'pool', 'yield', 'liquidity', 'trading', 'signal', 'whale', 'funding'],
  security: ['security', 'scan', 'rug', 'trust', 'risk', 'ofac', 'compliance', 'safety'],
  infra: ['infra', 'rpc', 'storage', 'ipfs', 'compute', 'engine'],
  agent: ['agent', 'autonomous', 'bot'],
  data: ['data', 'search', 'twitter', 'trend', 'news', 'feed', 'social', 'exa', 'web search'],
};

function guessCategory(text: string): X402Category {
  const lower = text.toLowerCase();
  let best: X402Category = 'data';
  let bestScore = 0;

  for (const [cat, keywords] of Object.entries(CATEGORY_KEYWORDS)) {
    const score = keywords.filter((kw) => lower.includes(kw)).length;
    if (score > bestScore) {
      bestScore = score;
      best = cat as X402Category;
    }
  }
  return best;
}

function parseParamLine(line: string): ServiceParam | null {
  const match = line.match(/^(\w+)\s*\((\w+)(?:,\s*(required|optional))?\)\s*[-–—:]?\s*(.+)?/);
  if (!match) return null;
  return {
    name: match[1],
    label: match[1].charAt(0).toUpperCase() + match[1].slice(1).replace(/_/g, ' '),
    type: match[2] === 'number' ? 'number' : 'string',
    required: match[3] === 'required',
    description: match[4]?.trim() || match[1],
  };
}

function extractParamsFromDescription(description: string): ServiceParam[] {
  const params: ServiceParam[] = [];
  const seen = new Set<string>();

  const patterns = [
    /\bby\s+(\w+)\s*\(([^)]+)\)/gi,
    /\b(\w+)\s*[:=]\s*\(([^)]+)\)/gi,
    /\bfor\s+(?:a\s+)?(\w+)\b/gi,
  ];

  for (const re of [patterns[0], patterns[1]]) {
    let m: RegExpExecArray | null;
    while ((m = re.exec(description)) !== null) {
      const name = m[1].toLowerCase();
      if (seen.has(name) || name.length < 2 || STOP_WORDS.has(name)) continue;
      seen.add(name);

      const hint = m[2] || '';
      const isNum = /^\d+=/.test(hint) || /\d+/.test(hint.split(',')[0]?.trim() || '');
      params.push({
        name,
        label: name.charAt(0).toUpperCase() + name.slice(1),
        type: isNum ? 'number' : 'string',
        required: false,
        description: hint ? `${name} (${hint.trim()})` : name,
      });
    }
  }

  return params;
}

const STOP_WORDS = new Set([
  'the', 'a', 'an', 'is', 'are', 'was', 'were', 'be', 'been',
  'have', 'has', 'had', 'do', 'does', 'did', 'will', 'would',
  'could', 'should', 'may', 'might', 'can', 'this', 'that',
  'with', 'from', 'into', 'use', 'get', 'set',
]);

function splitUrl(fullUrl: string): { baseUrl: string; path: string } {
  try {
    const url = new URL(fullUrl.trim());
    return {
      baseUrl: `${url.protocol}//${url.host}`,
      path: url.pathname,
    };
  } catch {
    const slashIdx = fullUrl.indexOf('/', fullUrl.indexOf('//') + 2);
    if (slashIdx > 0) {
      return {
        baseUrl: fullUrl.slice(0, slashIdx),
        path: fullUrl.slice(slashIdx),
      };
    }
    return { baseUrl: fullUrl, path: '/' };
  }
}

function deriveServiceName(url: string, docsUrl?: string): string {
  try {
    const parsed = new URL(url);
    const pathParts = parsed.pathname.split('/').filter(Boolean);

    const skipWords = new Set(['api', 'v1', 'v2', 'service', 'invoke', 'call', 'actions', 'run']);
    const meaningful = pathParts.filter((p) => !skipWords.has(p.toLowerCase()) && p.length > 1);

    if (meaningful.length > 0) {
      const last = meaningful[meaningful.length - 1];
      const servicePart = meaningful.length > 1 ? meaningful[meaningful.length - 2] : null;
      const raw = servicePart && servicePart !== last ? `${servicePart} ${last}` : last;
      return raw
        .replace(/[-_]/g, ' ')
        .split(' ')
        .map((w) => w.charAt(0).toUpperCase() + w.slice(1))
        .join(' ');
    }

    const host = parsed.hostname.replace(/^(www|api|registry)\./, '');
    const name = host.split('.')[0];
    return name.charAt(0).toUpperCase() + name.slice(1);
  } catch {
    return 'Custom Service';
  }
}

export function parseSkillText(text: string): ParsedSkill {
  const lines = text.split('\n').map((l) => l.trim()).filter(Boolean);
  const parsed: Record<string, string> = {};
  const descriptionLines: string[] = [];
  const paramLines: string[] = [];
  let inParams = false;

  for (const line of lines) {
    let matched = false;
    for (const [key, re] of Object.entries(LINE_PATTERNS)) {
      const m = line.match(re);
      if (m) {
        if (key === 'endpoint') {
          parsed.method = m[1].toUpperCase();
          parsed.endpointUrl = m[2].trim();
        } else if (key === 'params') {
          inParams = true;
          const p = parseParamLine(m[1].trim());
          if (p) paramLines.push(m[1].trim());
        } else {
          parsed[key] = m[1].trim();
        }
        matched = true;
        break;
      }
    }

    if (!matched) {
      if (inParams) {
        const p = parseParamLine(line);
        if (p) {
          paramLines.push(line);
          continue;
        }
        inParams = false;
      }
      descriptionLines.push(line);
    }
  }

  if (!parsed.endpointUrl) {
    throw new Error('Missing "Endpoint:" line. Format: Endpoint: POST https://example.com/api/call');
  }

  const method = (parsed.method === 'GET' ? 'GET' : 'POST') as 'GET' | 'POST';
  const { baseUrl, path } = splitUrl(parsed.endpointUrl);
  const description = descriptionLines
    .filter((l) => !l.startsWith('Use x402') && !l.startsWith('Ask me'))
    .join(' ')
    .trim() || `x402 service at ${baseUrl}`;

  let params: ServiceParam[] = paramLines
    .map(parseParamLine)
    .filter((p): p is ServiceParam => p !== null);

  if (params.length === 0) {
    params = extractParamsFromDescription(description);
  }

  if (parsed.model) {
    const hasModel = params.some((p) => p.name === 'model');
    if (!hasModel) {
      params.unshift({
        name: 'model',
        label: 'Model',
        type: 'string',
        required: true,
        description: `Model to use (default: ${parsed.model})`,
      });
    }
  }

  const allText = `${parsed.name || ''} ${description} ${parsed.endpointUrl}`;
  const lowerAll = allText.toLowerCase();
  const isGeneration = /generat|image|video|audio|tts|stt|transcri|render/.test(lowerAll);

  if (isGeneration && !params.some((p) => p.name === 'prompt')) {
    params.push({
      name: 'prompt',
      label: 'Prompt',
      type: 'string',
      required: true,
      description: 'Text prompt describing what to generate',
    });
  }

  if (/image/.test(lowerAll) && !params.some((p) => p.name === 'aspect_ratio')) {
    params.push({
      name: 'aspect_ratio',
      label: 'Aspect Ratio',
      type: 'string',
      required: false,
      description: 'Image aspect ratio (e.g. 1:1, 16:9, 9:16). Defaults to 1:1.',
    });
  }

  if (/search|query|lookup|find/.test(lowerAll) && !params.some((p) => ['q', 'query', 'prompt', 'input'].includes(p.name))) {
    params.push({
      name: 'q',
      label: 'Query',
      type: 'string',
      required: true,
      description: 'Search query',
    });
  }

  const name = parsed.name || deriveServiceName(parsed.endpointUrl, parsed.docs);
  const category = parsed.category
    ? (parsed.category.toLowerCase() as X402Category)
    : guessCategory(allText);

  const descWithModel = parsed.model
    ? `${description} Model: ${parsed.model}.`
    : description;

  return {
    name,
    description: descWithModel,
    method,
    fullUrl: parsed.endpointUrl.trim(),
    baseUrl,
    path,
    price: parsed.price || 'varies',
    params,
    category,
    skillUrl: parsed.skill,
    docsUrl: parsed.docs,
  };
}

export function skillToService(parsed: ParsedSkill): X402Service {
  const id = `custom_${parsed.baseUrl.replace(/[^a-z0-9]/gi, '_')}_${parsed.path.replace(/[^a-z0-9]/gi, '_')}`.toLowerCase();
  const endpointId = parsed.path.split('/').filter(Boolean).pop() || 'call';

  const hasModel = parsed.params.some((p) => p.name === 'model');
  const hasPrompt = parsed.params.some((p) => p.name === 'prompt');
  const isInvokeStyle = hasModel && hasPrompt;

  const endpoint: ServiceEndpoint = {
    id: endpointId,
    name: parsed.name,
    toolDescription: `${parsed.description} (x402 paid, ~${parsed.price}).`,
    method: parsed.method,
    path: parsed.path,
    params: parsed.params,
    ...(parsed.method === 'POST' && !isInvokeStyle ? { bodyTemplate: 'json-fields' as const } : {}),
    ...(isInvokeStyle ? { wrapInput: true } : {}),
  };

  return {
    id,
    name: parsed.name,
    description: parsed.description,
    category: parsed.category,
    baseUrl: parsed.baseUrl,
    priceRange: parsed.price,
    status: 'live',
    endpoints: [endpoint],
    isCustom: true,
  } as X402Service & { isCustom: boolean };
}
