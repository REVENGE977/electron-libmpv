declare module 'electron-libmpv' {
    namespace Mpv {
        export interface MpvEvent {
            type: 'start-file' | 'file-loaded' | 'playback-restart' | 'end-file' | 'property-change' | 'log-message';
            name?: string;
            value?: any;
            reason?: 'eof' | 'stop' | 'quit' | 'error' | 'redirect' | 'other';
            error?: string;
            prefix?: string;
            level?: string;
            text?: string;
        }

        export interface MpvNativePixmapPlane {
            stride: number;
            offset: number;
            size: number;
            fd: number;
        }

        export interface MpvSharedTextureHandle {
            ntHandle?: Buffer;
            ioSurface?: Buffer;
            nativePixmap?: {
                planes: MpvNativePixmapPlane[];
                modifier: string;
                supportsZeroCopyWebGpuImport: boolean;
            };
        }

        export interface MpvFrame {
            kind: 'sharedTexture' | 'software';
            slotId: number;
            generation: number;
            width: number;
            height: number;
            pixelFormat: 'bgra' | 'rgba' | 'rgbaf16';
            hdr: boolean;
            timestamp: number;
            handle?: MpvSharedTextureHandle;
            stride?: number;
            buffer?: Buffer;
        }

        export interface MpvOptions {
            libmpvPath?: string;
            configPath?: string;
            width?: number;
            height?: number;
            onEvent?: (event: MpvEvent) => void;
            onFrame?: (frame: MpvFrame) => void;
        }

        export interface MpvInitOptions {
            width?: number;
            height?: number;
            hdr?: boolean;
            preferGpu?: boolean;
        }

        export interface MpvInitResult {
            ok: boolean;
            backend?: string;
            sharedTexture?: boolean;
            hdrSupported?: boolean;
            libmpvPath?: string;
            gpuError?: string;
            error?: string;
        }

        export interface MpvBackendInfo {
            libmpvLoaded: boolean;
            libmpvPath: string;
            libmpvError: string;
            initialized: boolean;
            backend: string;
            sharedTexture: boolean;
            hdrSupported: boolean;
        }

        export type MpvObserveFormat = 'string' | 'double' | 'flag' | 'int64' | 'node';
    }

    class Mpv {
        constructor(options?: Mpv.MpvOptions);
        initialize(options?: Mpv.MpvInitOptions): Mpv.MpvInitResult;
        resize(width: number, height: number, hdr?: boolean): void;
        releaseFrame(slotId: number, generation?: number): void;
        enableSoftwareFallback(): boolean;
        command(...args: (string | number | boolean)[]): number;
        property(name: string, value: string | number | boolean): number;
        getRawProperty(name: string, format?: 'string' | 'node'): any;
        observeProperty(name: string, format?: Mpv.MpvObserveFormat): number;
        unobserveAll(): number;
        getBackendInfo(): Mpv.MpvBackendInfo;
        destroy(): void;
    }

    export = Mpv;
}
