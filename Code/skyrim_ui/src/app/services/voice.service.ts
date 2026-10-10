import { Injectable } from '@angular/core';
import { BehaviorSubject, combineLatest } from 'rxjs';
import { ClientService } from './client.service';
import { PlayerListService } from './player-list.service';
import { SettingService } from './setting.service';
import { StoreService } from './store.service';

const mutedKey = 'voice_muted';

/**
 * Voice chat's settings, sent to the game when they change and once when the menu loads (the game keeps none of its
 * own). Mutes are kept by player name: a player's id changes from one session to the next.
 */
@Injectable({
  providedIn: 'root',
})
export class VoiceService {
  public readonly mutedNames = new BehaviorSubject<string[]>(this.loadMuted());

  constructor(
    private readonly client: ClientService,
    private readonly playerListService: PlayerListService,
    private readonly settingService: SettingService,
    private readonly storeService: StoreService,
  ) {
    const settings = this.settingService.settings;
    settings.voiceEnabled.subscribe(enabled =>
      this.client.setVoiceEnabled(enabled),
    );
    settings.voiceVolume.subscribe(volume =>
      this.client.setVoiceVolume(Number(volume)),
    );
    combineLatest([this.playerListService.playerList, this.mutedNames]).subscribe(
      ([playerList, muted]) => {
        for (const player of playerList?.players ?? []) {
          this.client.setPlayerVoiceMuted(player.id, muted.includes(player.name));
        }
      },
    );
  }

  public toggleMute(name: string): void {
    const muted = this.mutedNames.getValue();
    const next = muted.includes(name)
      ? muted.filter(mutedName => mutedName !== name)
      : [...muted, name];
    this.storeService.set(mutedKey, JSON.stringify(next));
    this.mutedNames.next(next);
  }

  private loadMuted(): string[] {
    try {
      const stored = JSON.parse(this.storeService.get(mutedKey, '[]'));
      return Array.isArray(stored)
        ? stored.filter(name => typeof name === 'string')
        : [];
    } catch {
      return [];
    }
  }
}
