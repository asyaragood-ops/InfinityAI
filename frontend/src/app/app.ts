import { ChangeDetectionStrategy, Component, inject } from '@angular/core';
import { RouterLink, RouterLinkActive, RouterOutlet } from '@angular/router';
import { BackendService } from './core/backend.service';
import { ModelsService } from './core/models.service';
import { ThemeService } from './core/theme.service';
import { Icon, IconName } from './shared/icon';

@Component({
  selector: 'app-root',
  imports: [RouterOutlet, RouterLink, RouterLinkActive, Icon],
  changeDetection: ChangeDetectionStrategy.OnPush,
  templateUrl: './app.html',
  styleUrl: './app.css',
})
export class App {
  protected readonly theme = inject(ThemeService);
  protected readonly models = inject(ModelsService);
  /** Создаём сразу: при запуске сайта ключи из «Настроек» отправляются на бэкенд. */
  protected readonly backend = inject(BackendService);

  protected readonly nav: { path: string; label: string; icon: IconName }[] = [
    { path: '/chat', label: 'Чаты', icon: 'chat' },
    { path: '/models', label: 'Модели', icon: 'models' },
    { path: '/settings', label: 'Настройки', icon: 'settings' },
  ];
}
