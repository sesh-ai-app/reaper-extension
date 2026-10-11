import { configs, plugins } from 'eslint-config-airbnb-extended';
import globals from 'globals';

/**
 * Airbnb base, its TypeScript layer, and its React layer — then the project's own
 * deviations: tab indentation everywhere, and no maximum line length.
 */
export default [
	{
		ignores: ['dist/**', 'node_modules/**'],
	},

	// The Airbnb config groups reference these plugins by name but leave registration to the
	// consumer, so they have to be installed into the config before the groups are spread.
	plugins.stylistic,
	plugins.importX,
	plugins.node,
	plugins.typescriptEslint,
	plugins.react,
	plugins.reactA11y,
	plugins.reactHooks,

	...configs.base.recommended,
	...configs.base.typescript,
	...configs.react.recommended,
	...configs.react.typescript,

	{
		name: 'sesh-ai/language-options',
		languageOptions: {
			globals: {
				...globals.browser,
			},
		},
	},

	{
		name: 'sesh-ai/code-style',
		rules: {
			// Tab indentation across every language in this repository.
			'@stylistic/no-tabs': ['error', { allowIndentationTabs: true }],
			'@stylistic/indent': [
				'error',
				'tab',
				{
					// JSX indentation is governed by react/jsx-indent below.
					ignoredNodes: [
						'JSXElement',
						'JSXElement > *',
						'JSXAttribute',
						'JSXIdentifier',
						'JSXNamespacedName',
						'JSXMemberExpression',
						'JSXSpreadAttribute',
						'JSXExpressionContainer',
						'JSXOpeningElement',
						'JSXClosingElement',
						'JSXFragment',
						'JSXOpeningFragment',
						'JSXClosingFragment',
						'JSXText',
						'JSXEmptyExpression',
						'JSXSpreadChild',
					],
					SwitchCase: 1,
					VariableDeclarator: 1,
					outerIIFEBody: 1,
					MemberExpression: 1,
					FunctionDeclaration: { parameters: 1, body: 1 },
					FunctionExpression: { parameters: 1, body: 1 },
					StaticBlock: { body: 1 },
					CallExpression: { arguments: 1 },
					ArrayExpression: 1,
					ObjectExpression: 1,
					ImportDeclaration: 1,
					flatTernaryExpressions: false,
					offsetTernaryExpressions: false,
					ignoreComments: false,
				},
			],
			'react/jsx-indent': ['error', 'tab'],
			'react/jsx-indent-props': ['error', 'tab'],

			// Readability over brevity — descriptive names are worth a long line.
			'@stylistic/max-len': 'off',

			// The react-jsx transform makes the React import unnecessary.
			'react/react-in-jsx-scope': 'off',

			// Console output is how the UI surfaces diagnostics to a producer running REAPER
			// with a console attached.
			'no-console': 'off',

			// Default exports are not required (code-style steering).
			'import-x/prefer-default-export': 'off',
		},
	},

	{
		name: 'sesh-ai/build-tooling',
		files: ['vite.config.ts', 'eslint.config.js'],
		rules: {
			'import-x/no-extraneous-dependencies': ['error', { devDependencies: true }],
		},
	},

	{
		name: 'sesh-ai/tests',
		files: ['**/*.test.ts', '**/*.test.tsx', '**/*.fixture.ts'],
		rules: {
			// Vitest, fast-check, and Testing Library are devDependencies by definition — none
			// of them is bundled into `dist`.
			'import-x/no-extraneous-dependencies': ['error', { devDependencies: true }],

			// Airbnb bans `for...of` because of the regenerator runtime it used to imply. A
			// property test that runs its full set of cases per bridge message name is the
			// shape this ban has no answer for: `forEach` over ten names with an `fc.assert`
			// inside reads worse and is no cheaper.
			'no-restricted-syntax': 'off',
		},
	},
];
