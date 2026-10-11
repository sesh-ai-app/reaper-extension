import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';

import App from './App';
import './i18n';

const rootElement = document.getElementById('root');

if (!rootElement) {
	throw new Error('The #root element is missing from index.html — the UI cannot mount.');
}

createRoot(rootElement).render(
	<StrictMode>
		<App />
	</StrictMode>,
);
