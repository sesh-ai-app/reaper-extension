/**
 * The CEF bridge message client.
 *
 * `contract.ts` is the mirror of `src/ui/ui_host.h`, `payloads.ts` the types derived from
 * the vendored schemas, `transport.ts` the two globals CEF provides, and `receive.ts` and
 * `send.ts` the two directions. Components should reach for the conversation state hook
 * rather than these directly; this barrel exists for the hook and for the suite.
 */

export * from './contract';
export * from './payloads';
export * from './receive';
export * from './send';
export * from './transport';
