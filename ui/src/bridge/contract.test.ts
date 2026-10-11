/**
 * The test that makes the bridge contract a contract.
 *
 * `src/ui/ui_host.h` declares the ten message names, their direction, the schema each
 * payload answers to, the `view:` namespace, and the camelCase rule. `contract.ts`
 * restates all of it for a toolchain that cannot read a C++ header. Nothing links the two
 * at build time — different compilers, no shared artefact — so a rename on one side is not
 * a build failure on either. It is a panel that quietly stops showing one thing.
 *
 * This file parses the header and fails when they disagree. It is the reason the names
 * live in `ui_host.h` rather than in `cef_browser_host.h`, which needs a CEF distribution
 * and which no suite compiles: a contract nothing can assert is the contract that drifts.
 *
 * **Validates: Requirements 15.2, 15.4**
 */

import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import { describe, expect, it } from 'vitest';

import {
	allBridgeMessageNames,
	bridgeFieldNamingConvention,
	bridgeMessageDirectionFor,
	bridgeMessageSchemaPathFor,
	bridgeMessagesFromJavaScript,
	bridgeMessagesToJavaScript,
	vendoredSchemaProtocolVersion,
	viewBridgeMessageNamespace,
} from './contract';

const uiHostHeaderPath = fileURLToPath(new URL('../../../src/ui/ui_host.h', import.meta.url));
const schemaBundlePath = fileURLToPath(new URL('../../../schemas/', import.meta.url));
const schemaManifestPath = `${schemaBundlePath}MANIFEST.json`;

const uiHostHeader = readFileSync(uiHostHeaderPath, 'utf8');

/**
 * Every `inline constexpr std::string_view name{"value"};` in the header, including the
 * ones whose value sits on the following line. These are the constants the two halves have
 * to agree on, and reading them out of the header is what keeps this test honest — a
 * hand-copied list here would drift in exactly the way the production code must not.
 */
function parseStringViewConstants(header: string): ReadonlyMap<string, string> {
	const constants = new Map<string, string>();
	const pattern = /inline constexpr std::string_view\s+(\w+)\s*\{\s*"([^"]*)"\s*\}/gu;

	for (const match of header.matchAll(pattern)) {
		const [, constantName, value] = match;

		if (constantName !== undefined && value !== undefined) {
			constants.set(constantName, value);
		}
	}

	return constants;
}

/** The constant names inside one of the `std::array` initialisers, in declaration order. */
function parseNameArray(header: string, arrayName: string): readonly string[] {
	const pattern = new RegExp(
		`inline constexpr std::array<std::string_view,\\s*\\d+>\\s+${arrayName}\\{([^}]*)\\}`,
		'u',
	);
	const match = pattern.exec(header);

	expect(match, `${arrayName} is declared in ui_host.h`).not.toBeNull();

	return (match?.[1] ?? '')
		.split(',')
		.map((entry) => entry.trim())
		.filter((entry) => entry.length > 0 && !entry.startsWith('//'));
}

/**
 * The `bridge_message_schema_path_for` mapping, read out of the if-chain. Names accumulate
 * until a `return`, so the one branch that answers for both `confirm:approve` and
 * `confirm:reject` maps both.
 */
function parseSchemaPathMapping(header: string): ReadonlyMap<string, string> {
	const functionStart = header.indexOf(
		'constexpr std::string_view bridge_message_schema_path_for',
	);

	expect(functionStart, 'bridge_message_schema_path_for is declared in ui_host.h')
		.toBeGreaterThan(-1);

	const body = header.slice(functionStart);
	const bodyEnd = body.indexOf('\n\t}');
	const functionBody = bodyEnd > 0 ? body.slice(0, bodyEnd) : body;

	const mapping = new Map<string, string>();
	let pendingNames: string[] = [];

	for (const match of functionBody.matchAll(/message_name == (\w+)|return "([^"]+)"/gu)) {
		const [, comparedConstant, schemaPath] = match;

		if (comparedConstant !== undefined) {
			pendingNames.push(comparedConstant);
		} else if (schemaPath !== undefined) {
			for (const constantName of pendingNames) {
				mapping.set(constantName, schemaPath);
			}

			pendingNames = [];
		}
	}

	return mapping;
}

const headerConstants = parseStringViewConstants(uiHostHeader);

function valueOf(constantName: string): string {
	const value = headerConstants.get(constantName);

	expect(value, `${constantName} is declared in ui_host.h`).toBeTypeOf('string');

	return value ?? '';
}

describe('the bridge contract agrees with ui_host.h', () => {
	it('parsed the header at all', () => {
		// A regex that silently matched nothing would make every assertion below vacuous.
		expect(uiHostHeader.length).toBeGreaterThan(1000);
		expect(headerConstants.size).toBeGreaterThanOrEqual(12);
	});

	it('names the same ten bridge messages, in the same order', () => {
		const headerNames = parseNameArray(uiHostHeader, 'all_bridge_message_names').map(valueOf);

		expect(headerNames).toStrictEqual([...allBridgeMessageNames]);
	});

	it('sends the same seven messages to JavaScript', () => {
		const headerNames = parseNameArray(uiHostHeader, 'bridge_messages_to_javascript').map(valueOf);

		expect(headerNames).toStrictEqual([...bridgeMessagesToJavaScript]);
	});

	it('receives the same three messages from JavaScript', () => {
		const headerNames = parseNameArray(
			uiHostHeader,
			'bridge_messages_from_javascript',
		).map(valueOf);

		expect(headerNames).toStrictEqual([...bridgeMessagesFromJavaScript]);
	});

	it('agrees on the direction of every name', () => {
		const toJavaScript = new Set(
			parseNameArray(uiHostHeader, 'bridge_messages_to_javascript').map(valueOf),
		);

		for (const messageName of allBridgeMessageNames) {
			expect(bridgeMessageDirectionFor(messageName)).toBe(
				toJavaScript.has(messageName) ? 'toJavaScript' : 'fromJavaScript',
			);
		}
	});

	it('maps every name to the schema path the header maps it to', () => {
		const mapping = parseSchemaPathMapping(uiHostHeader);

		expect(mapping.size).toBe(allBridgeMessageNames.length);

		for (const [constantName, schemaPath] of mapping) {
			expect(bridgeMessageSchemaPathFor(valueOf(constantName))).toBe(schemaPath);
		}
	});

	it('agrees on the view namespace and the field naming convention', () => {
		expect(viewBridgeMessageNamespace).toBe(valueOf('view_bridge_message_namespace'));
		expect(bridgeFieldNamingConvention).toBe(valueOf('bridge_field_naming_convention'));
	});
});

describe('the bridge contract agrees with the vendored schema bundle', () => {
	it('points every message at a schema that exists', () => {
		for (const messageName of allBridgeMessageNames) {
			const schemaPath = bridgeMessageSchemaPathFor(messageName);

			expect(schemaPath, `${messageName} has a schema path`).not.toBe('');
			expect(
				existsSync(`${schemaBundlePath}${schemaPath}`),
				`${schemaPath} exists in the vendored bundle`,
			).toBe(true);
		}
	});

	it('was derived from the bundle currently vendored', () => {
		const manifest: unknown = JSON.parse(readFileSync(schemaManifestPath, 'utf8'));
		const { protocolVersion } = (manifest as { protocolVersion?: unknown });

		// A regenerated bundle may have moved a field these types name. Failing here is the
		// prompt to re-read the six view model schemas, not a formality.
		expect(protocolVersion).toBe(vendoredSchemaProtocolVersion);
	});

	it('keeps the snake_case confirmation request out of the UI', () => {
		// `messages/confirmation-request.schema.json` is the one protocol payload spelled
		// snake_case, and it is a server-side contract this spec does not change. The
		// coordinator decodes it and publishes `view:confirmation_pending` in camelCase, so
		// the pending view's schema must carry the camelCase spellings and not the others.
		const pendingSchema = readFileSync(
			`${schemaBundlePath}bridge-messages/confirmation-pending.schema.json`,
			'utf8',
		);
		const properties: unknown = (
			JSON.parse(pendingSchema) as { properties?: unknown }
		).properties;
		const fieldNames = Object.keys(properties as Record<string, unknown>);

		expect(fieldNames).toContain('actionSummary');
		expect(fieldNames).toContain('riskLevel');
		expect(fieldNames).not.toContain('action_summary');
		expect(fieldNames).not.toContain('risk_level');
	});

	it('has no snake_case field name anywhere in the view model schemas', () => {
		// The camelCase rule, asserted over the bundle rather than per schema, so a
		// regenerated view model that arrived snake_case is caught here.
		const viewModelSchemaPaths = allBridgeMessageNames
			.map((messageName) => bridgeMessageSchemaPathFor(messageName))
			.filter((schemaPath) => schemaPath.startsWith('bridge-messages/'));

		expect(viewModelSchemaPaths).toHaveLength(6);

		for (const schemaPath of viewModelSchemaPaths) {
			const schema: unknown = JSON.parse(
				readFileSync(`${schemaBundlePath}${schemaPath}`, 'utf8'),
			);
			const fieldNames = Object.keys(
				(schema as { properties: Record<string, unknown> }).properties,
			);

			for (const fieldName of fieldNames) {
				expect(fieldName, `${schemaPath} field ${fieldName}`).not.toMatch(/_/u);
			}
		}
	});
});
