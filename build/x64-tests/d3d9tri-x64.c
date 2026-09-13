/* d3d9tri-x64 — minimal Direct3D 9 smoke test, x86-64 PE.
 *
 * Exists to exercise the path the DXMT cube test cannot reach: d3d9 ->
 * wined3d -> (with WINE_D3D_CONFIG=renderer=vulkan) adapter_vk -> winevulkan
 * -> win32u -> winios.drv vulkan_surface_create -> vkCreateMetalSurfaceEXT ->
 * MoltenVK -> the compositor's CAMetalLayer.
 *
 * Deliberately tiny: create a device, clear to a cycling colour, Present.
 * Anything that reaches the screen means the whole chain works. Every step
 * reports to stdout so a failure localises itself in madeira-log.txt.
 */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

static LRESULT CALLBACK wndproc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    if (msg == WM_DESTROY) { PostQuitMessage( 0 ); return 0; }
    return DefWindowProcA( hwnd, msg, wp, lp );
}

int main( void )
{
    WNDCLASSA wc = { 0 };
    HWND hwnd;
    IDirect3D9 *d3d;
    IDirect3DDevice9 *dev = NULL;
    D3DPRESENT_PARAMETERS pp = { 0 };
    D3DADAPTER_IDENTIFIER9 id = { 0 };
    HRESULT hr;
    int frame;

    setvbuf( stdout, NULL, _IONBF, 0 );
    printf( "[d3d9tri] start\n" );

    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA( NULL );
    wc.lpszClassName = "d3d9tri";
    RegisterClassA( &wc );

    hwnd = CreateWindowA( "d3d9tri", "d3d9tri", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                          0, 0, 640, 480, NULL, NULL, wc.hInstance, NULL );
    if (!hwnd) { printf( "[d3d9tri] CreateWindow FAILED %lu\n", GetLastError() ); return 1; }
    printf( "[d3d9tri] hwnd=%p\n", (void *)hwnd );

    if (!(d3d = Direct3DCreate9( D3D_SDK_VERSION )))
    {
        printf( "[d3d9tri] Direct3DCreate9 FAILED -- no d3d9 at all\n" );
        return 1;
    }
    printf( "[d3d9tri] Direct3DCreate9 ok, adapters=%u\n",
            IDirect3D9_GetAdapterCount( d3d ) );

    if (SUCCEEDED(IDirect3D9_GetAdapterIdentifier( d3d, D3DADAPTER_DEFAULT, 0, &id )))
        printf( "[d3d9tri] adapter: %s / %s\n", id.Description, id.Driver );

    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow = hwnd;

    hr = IDirect3D9_CreateDevice( d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                  D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev );
    if (FAILED(hr) || !dev)
    {
        printf( "[d3d9tri] CreateDevice FAILED hr=0x%08lx -- device/swapchain is "
                "where the vulkan surface would be created\n", (unsigned long)hr );
        IDirect3D9_Release( d3d );
        return 1;
    }
    printf( "[d3d9tri] CreateDevice ok, dev=%p\n", (void *)dev );

    for (frame = 0; frame < 600; frame++)
    {
        MSG msg;
        D3DCOLOR c = D3DCOLOR_XRGB( (frame * 3) & 0xff, 64, 255 - ((frame * 3) & 0xff) );

        while (PeekMessageA( &msg, NULL, 0, 0, PM_REMOVE ))
        {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage( &msg );
            DispatchMessageA( &msg );
        }

        hr = IDirect3DDevice9_Clear( dev, 0, NULL, D3DCLEAR_TARGET, c, 1.0f, 0 );
        if (FAILED(hr) && frame == 0) printf( "[d3d9tri] Clear FAILED hr=0x%08lx\n", (unsigned long)hr );

        IDirect3DDevice9_BeginScene( dev );
        IDirect3DDevice9_EndScene( dev );

        hr = IDirect3DDevice9_Present( dev, NULL, NULL, NULL, NULL );
        if (FAILED(hr) && frame == 0) printf( "[d3d9tri] Present FAILED hr=0x%08lx\n", (unsigned long)hr );

        if (frame == 0)   printf( "[d3d9tri] first frame presented\n" );
        if (frame == 299) printf( "[d3d9tri] 300 frames presented\n" );
        Sleep( 16 );
    }

done:
    printf( "[d3d9tri] done, releasing\n" );
    IDirect3DDevice9_Release( dev );
    IDirect3D9_Release( d3d );
    printf( "[d3d9tri] exit ok\n" );
    return 0;
}
