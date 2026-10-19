import { Routes } from '@angular/router';

export const routes: Routes = [
  { path: '', pathMatch: 'full', redirectTo: 'chat' },
  { path: 'chat', title: 'Чаты · infinityAI', loadComponent: () => import('./pages/chat/chat-page').then((m) => m.ChatPage) },
  { path: 'models', title: 'Модели · infinityAI', loadComponent: () => import('./pages/models/models-page').then((m) => m.ModelsPage) },
  { path: 'settings', title: 'Настройки · infinityAI', loadComponent: () => import('./pages/settings/settings-page').then((m) => m.SettingsPage) },
  { path: '**', redirectTo: 'chat' },
];
