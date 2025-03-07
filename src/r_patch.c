/*
========================================================================

                               DOOM Retro
         The classic, refined DOOM source port. For Windows PC.

========================================================================

  Copyright © 1993-2022 by id Software LLC, a ZeniMax Media company.
  Copyright © 2013-2022 by Brad Harding <mailto:brad@doomretro.com>.

  DOOM Retro is a fork of Chocolate DOOM. For a list of acknowledgments,
  see <https://github.com/bradharding/doomretro/wiki/ACKNOWLEDGMENTS>.

  This file is a part of DOOM Retro.

  DOOM Retro is free software: you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the
  Free Software Foundation, either version 3 of the license, or (at your
  option) any later version.

  DOOM Retro is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with DOOM Retro. If not, see <https://www.gnu.org/licenses/>.

  DOOM is a registered trademark of id Software LLC, a ZeniMax Media
  company, in the US and/or other countries, and is used without
  permission. All other trademarks are the property of their respective
  holders. DOOM Retro is in no way affiliated with nor endorsed by
  id Software.

========================================================================
*/

#include "c_console.h"
#include "doomstat.h"
#include "i_swap.h"
#include "r_main.h"
#include "w_wad.h"
#include "z_zone.h"

//
// Patches.
// A patch holds one or more columns.
// Patches are used for sprites and all masked pictures,
// and we compose textures from the TEXTURE1/2 lists  of patches.
//

// Re-engineered patch support
static rpatch_t *patches;
static rpatch_t *texture_composites;

static short    BIGDOOR7;
static short    FIREBLU1;
static short    SKY1;
static short    STEP2;

static bool getIsSolidAtSpot(const column_t *column, int spot)
{
    if (!column)
        return false;

    while (column->topdelta != 0xFF)
    {
        if (spot < column->topdelta)
            return false;

        if (spot <= column->topdelta + column->length)
            return true;

        column = (const column_t *)((const byte *)column + 3 + column->length + 1);
    }

    return false;
}

// Checks if the lump can be a DOOM patch
static bool CheckIfPatch(int lump)
{
    const int   size = W_LumpLength(lump);
    bool        result = false;

    if (size >= 13)
    {
        const patch_t       *patch = W_CacheLumpNum(lump);
        const unsigned char *magic = (const unsigned char *)patch;

        if (magic[0] == 0x89 && magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G')
            C_Warning(1, "The " BOLD("%.8s") " patch is an unsupported PNG lump and will be ignored.", lumpinfo[lump]->name);
        else
        {
            const short width = SHORT(patch->width);
            const short height = SHORT(patch->height);

            if ((result = (width > 0 && width <= 16384 && width < size / 4 && height > 0 && height <= 16384)))
                // The dimensions seem like they might be valid for a patch, so
                // check the column directory for extra security. All columns
                // must begin after the column directory, and none of them must
                // point past the end of the patch.
                for (int x = 0; x < width; x++)
                {
                    const unsigned int  offset = LONG(patch->columnoffset[x]);

                    // Need one byte for an empty column (but there's patches that don't know that!)
                    if (offset < (unsigned int)width * 4 + 8 || offset >= (unsigned int)size)
                    {
                        result = false;

                        if (lumpinfo[lump]->size > 0)
                            C_Warning(1, "The " BOLD("%.8s") " patch is in an unknown format and will be ignored.", lumpinfo[lump]->name);

                        break;
                    }
                }
        }

        W_ReleaseLumpNum(lump);
    }

    return result;
}

static void createPatch(int patchNum)
{
    rpatch_t            *patch;
    const patch_t       *oldPatch;
    const column_t      *oldColumn;
    int                 pixelDataSize;
    int                 columnsDataSize;
    int                 postsDataSize;
    int                 dataSize;
    int                 *numPostsInColumn;
    int                 numPostsTotal;
    const unsigned char *oldColumnPixelData;

    if (!CheckIfPatch(patchNum))
        patchNum = W_GetNumForName("TNT1A0");

    oldPatch = W_CacheLumpNum(patchNum);
    patch = &patches[patchNum];
    patch->width = SHORT(oldPatch->width);
    patch->widthmask = 0;
    patch->height = SHORT(oldPatch->height);
    patch->leftoffset = SHORT(oldPatch->leftoffset);
    patch->topoffset = SHORT(oldPatch->topoffset);

    // work out how much memory we need to allocate for this patch's data
    pixelDataSize = ((patch->width * patch->height + 4) & ~3);
    columnsDataSize = patch->width * sizeof(rcolumn_t);

    // count the number of posts in each column
    numPostsInColumn = malloc(patch->width * sizeof(int));
    numPostsTotal = 0;

    for (int x = 0; x < patch->width; x++)
    {
        oldColumn = (const column_t *)((const byte *)oldPatch + LONG(oldPatch->columnoffset[x]));
        numPostsInColumn[x] = 0;

        while (oldColumn->topdelta != 0xFF)
        {
            numPostsInColumn[x]++;
            numPostsTotal++;
            oldColumn = (const column_t *)((const byte *)oldColumn + oldColumn->length + 4);
        }
    }

    postsDataSize = numPostsTotal * sizeof(rpost_t);

    // allocate our data chunk
    dataSize = pixelDataSize + columnsDataSize + postsDataSize;
    patch->data = Z_Calloc(1, dataSize, PU_CACHE, (void **)&patch->data);

    // set out pixel, column, and post pointers into our data array
    patch->pixels = patch->data;
    patch->columns = (rcolumn_t *)((unsigned char *)patch->pixels + pixelDataSize);
    patch->posts = (rpost_t *)((unsigned char *)patch->columns + columnsDataSize);

    // sanity check that we've got all the memory allocated we need
    assert((((byte *)patch->posts + numPostsTotal * sizeof(rpost_t)) - (byte *)patch->data) == dataSize);

    memset(patch->pixels, 0xFF, (size_t)patch->width * patch->height);

    // fill in the pixels, posts, and columns
    for (int x = 0, numPostsUsedSoFar = 0; x < patch->width; x++)
    {
        int top = -1;

        oldColumn = (const column_t *)((const byte *)oldPatch + LONG(oldPatch->columnoffset[x]));

        // setup the column's data
        patch->columns[x].pixels = &patch->pixels[x * patch->height];
        patch->columns[x].numposts = numPostsInColumn[x];
        patch->columns[x].posts = patch->posts + numPostsUsedSoFar;

        while (oldColumn->topdelta != 0xFF)
        {
            int len = oldColumn->length;

            // e6y: support for DeePsea's true tall patches
            if (oldColumn->topdelta <= top)
                top += oldColumn->topdelta;
            else
                top = oldColumn->topdelta;

            // Clip posts that extend past the bottom
            if (top + oldColumn->length > patch->height)
                len = patch->height - top;

            if (len > 0)
            {
                // set up the post's data
                patch->posts[numPostsUsedSoFar].topdelta = top;
                patch->posts[numPostsUsedSoFar].length = len;

                // fill in the post's pixels
                oldColumnPixelData = (const byte *)oldColumn + 3;

                for (int y = 0; y < len; y++)
                    patch->pixels[x * patch->height + top + y] = oldColumnPixelData[y];
            }

            oldColumn = (const column_t *)((const byte *)oldColumn + oldColumn->length + 4);
            numPostsUsedSoFar++;
        }
    }

    // copy the patch image down and to the right where there are
    // holes to eliminate the black halo from bilinear filtering
    for (int x = 0; x < patch->width; x++)
    {
        const rcolumn_t *column = R_GetPatchColumnClamped(patch, x);
        const rcolumn_t *prevColumn = R_GetPatchColumnClamped(patch, x - 1);

        if (column->pixels[0] == 0xFF)
        {
            // force the first pixel (which is a hole), to use
            // the color from the next solid spot in the column
            for (int y = 0; y < patch->height; y++)
                if (column->pixels[y] != 0xFF)
                {
                    column->pixels[0] = column->pixels[y];
                    break;
                }
        }

        // copy from above or to the left
        for (int y = 1; y < patch->height; y++)
        {
            if (getIsSolidAtSpot(oldColumn, y))
                continue;

            if (column->pixels[y] != 0xFF)
                continue;

            // this pixel is a hole
            if (x && prevColumn->pixels[y - 1] != 0xFF)
                column->pixels[y] = prevColumn->pixels[y];  // copy the color from the left
            else
                column->pixels[y] = column->pixels[y - 1];  // copy the color from above
        }
    }

    W_ReleaseLumpNum(patchNum);
    free(numPostsInColumn);
}

typedef struct
{
    unsigned short  patches;
    unsigned short  posts;
    unsigned short  posts_used;
} count_t;

static void switchPosts(rpost_t *post1, rpost_t *post2)
{
    rpost_t dummy;

    dummy.topdelta = post1->topdelta;
    dummy.length = post1->length;
    post1->topdelta = post2->topdelta;
    post1->length = post2->length;
    post2->topdelta = dummy.topdelta;
    post2->length = dummy.length;
}

static void removePostFromColumn(rcolumn_t *column, int post)
{
    if (post < column->numposts)
        for (int i = post; i < column->numposts - 1; i++)
        {
            rpost_t *post1 = &column->posts[i];
            rpost_t *post2 = &column->posts[i + 1];

            post1->topdelta = post2->topdelta;
            post1->length = post2->length;
        }

    column->numposts--;
}

static void createTextureCompositePatch(int id)
{
    rpatch_t            *composite_patch = &texture_composites[id];
    const texture_t     *texture = textures[id];
    const texpatch_t    *texpatch;
    int                 patchNum;
    const patch_t       *oldPatch;
    const column_t      *oldColumn;
    int                 count;
    int                 pixelDataSize;
    int                 columnsDataSize;
    int                 postsDataSize;
    int                 dataSize;
    int                 numPostsTotal;
    const unsigned char *oldColumnPixelData;
    count_t             *countsInColumn;

    composite_patch->width = texture->width;
    composite_patch->height = texture->height;
    composite_patch->widthmask = texture->widthmask;
    composite_patch->leftoffset = 0;
    composite_patch->topoffset = 0;

    // work out how much memory we need to allocate for this patch's data
    pixelDataSize = ((composite_patch->width * composite_patch->height + 4) & ~3);
    columnsDataSize = composite_patch->width * sizeof(rcolumn_t);

    // count the number of posts in each column
    countsInColumn = (count_t *)calloc(composite_patch->width, sizeof(count_t));
    numPostsTotal = 0;

    for (int i = 0; i < texture->patchcount; i++)
    {
        texpatch = &texture->patches[i];
        patchNum = texpatch->patch;

        if (!CheckIfPatch(patchNum))
            patchNum = W_GetNumForName("TNT1A0");

        oldPatch = (const patch_t *)W_CacheLumpNum(patchNum);

        for (int x = 0; x < SHORT(oldPatch->width); x++)
        {
            int tx = texpatch->originx + x;

            if (tx < 0)
                continue;

            if (tx >= composite_patch->width)
                break;

            countsInColumn[tx].patches++;

            oldColumn = (const column_t *)((const byte *)oldPatch + LONG(oldPatch->columnoffset[x]));

            while (oldColumn->topdelta != 0xFF)
            {
                countsInColumn[tx].posts++;
                numPostsTotal++;
                oldColumn = (const column_t *)((const byte *)oldColumn + oldColumn->length + 4);
            }
        }

        W_ReleaseLumpNum(patchNum);
    }

    postsDataSize = numPostsTotal * sizeof(rpost_t);

    // allocate our data chunk
    dataSize = pixelDataSize + columnsDataSize + postsDataSize;
    composite_patch->data = Z_Calloc(1, dataSize, PU_STATIC, (void **)&composite_patch->data);

    // set out pixel, column, and post pointers into our data array
    composite_patch->pixels = composite_patch->data;
    composite_patch->columns = (rcolumn_t *)((unsigned char *)composite_patch->pixels + pixelDataSize);
    composite_patch->posts = (rpost_t *)((unsigned char *)composite_patch->columns + columnsDataSize);

    // sanity check that we've got all the memory allocated we need
    assert((((byte *)composite_patch->posts + numPostsTotal * sizeof(rpost_t)) - (byte *)composite_patch->data) == dataSize);

    memset(composite_patch->pixels, 0xFF, (size_t)composite_patch->width * composite_patch->height);

    for (int x = 0, numPostsUsedSoFar = 0; x < texture->width; x++)
    {
        // setup the column's data
        composite_patch->columns[x].pixels = &composite_patch->pixels[x * composite_patch->height];
        composite_patch->columns[x].numposts = countsInColumn[x].posts;
        composite_patch->columns[x].posts = composite_patch->posts + numPostsUsedSoFar;
        numPostsUsedSoFar += countsInColumn[x].posts;
    }

    // fill in the pixels, posts, and columns
    for (int i = 0; i < texture->patchcount; i++)
    {
        texpatch = &texture->patches[i];
        patchNum = texpatch->patch;

        if (!CheckIfPatch(patchNum))
            patchNum = W_GetNumForName("TNT1A0");

        oldPatch = (const patch_t *)W_CacheLumpNum(patchNum);

        for (int x = 0; x < SHORT(oldPatch->width); x++)
        {
            int         top = -1;
            const int   tx = texpatch->originx + x;

            if (tx < 0)
                continue;

            if (tx >= composite_patch->width)
                break;

            oldColumn = (const column_t *)((const byte *)oldPatch + LONG(oldPatch->columnoffset[x]));

            while (oldColumn->topdelta != 0xFF)
            {
                int     oy = texpatch->originy;
                rpost_t *post = &composite_patch->columns[tx].posts[countsInColumn[tx].posts_used];

                // e6y: support for DeePsea's true tall patches
                if (oldColumn->topdelta <= top)
                    top += oldColumn->topdelta;
                else
                    top = oldColumn->topdelta;

                oldColumnPixelData = (const byte *)oldColumn + 3;
                count = oldColumn->length;

                // [BH] use incorrect y-origin for certain textures
                if (id == BIGDOOR7 || id == FIREBLU1 || id == SKY1 || (id == STEP2 && modifiedgame))
                    oy = 0;

                // set up the post's data
                post->topdelta = top + oy;
                post->length = count;

                if (post->topdelta + post->length > composite_patch->height)
                {
                    if (post->topdelta > composite_patch->height)
                        post->length = 0;
                    else
                        post->length = composite_patch->height - post->topdelta;
                }

                if (post->topdelta < 0)
                {
                    if (post->topdelta + post->length <= 0)
                        post->length = 0;
                    else
                        post->length -= post->topdelta;

                    post->topdelta = 0;
                }

                // fill in the post's pixels
                for (int y = 0; y < count; y++)
                {
                    const int   ty = oy + top + y;

                    if (ty < 0)
                        continue;

                    if (ty >= composite_patch->height)
                        break;

                    composite_patch->pixels[tx * composite_patch->height + ty] = oldColumnPixelData[y];
                }

                oldColumn = (const column_t *)((const byte *)oldColumn + oldColumn->length + 4);
                countsInColumn[tx].posts_used++;
                assert(countsInColumn[tx].posts_used <= countsInColumn[tx].posts);
            }
        }

        W_ReleaseLumpNum(patchNum);
    }

    for (int x = 0; x < texture->width; x++)
    {
        rcolumn_t   *column;
        int         i = 0;

        if (countsInColumn[x].patches <= 1)
            continue;

        // cleanup posts on multipatch columns
        column = &composite_patch->columns[x];

        while (i < column->numposts - 1)
        {
            rpost_t *post1 = &column->posts[i];
            rpost_t *post2 = &column->posts[i + 1];

            if (post2->topdelta - post1->topdelta < 0)
                switchPosts(post1, post2);

            if (post1->topdelta + post1->length >= post2->topdelta)
            {
                const int   length = post1->length + post2->length - (post1->topdelta + post1->length - post2->topdelta);

                if (post1->length < length)
                    post1->length = length;

                removePostFromColumn(column, i + 1);
                i = 0;

                continue;
            }

            i++;
        }
    }

    // copy the patch image down and to the right where there are
    // holes to eliminate the black halo from bilinear filtering
    for (int x = 0; x < composite_patch->width; x++)
    {
        const rcolumn_t *column = R_GetPatchColumnClamped(composite_patch, x);
        const rcolumn_t *prevColumn = R_GetPatchColumnClamped(composite_patch, x - 1);

        if (column->pixels[0] == 0xFF)
        {
            // force the first pixel (which is a hole), to use
            // the color from the next solid spot in the column
            for (int y = 0; y < composite_patch->height; y++)
                if (column->pixels[y] != 0xFF)
                {
                    column->pixels[0] = column->pixels[y];
                    break;
                }
        }

        // copy from above or to the left
        for (int y = 1; y < composite_patch->height; y++)
        {
            if (column->pixels[y] != 0xFF)
                continue;

            // this pixel is a hole
            if (x && prevColumn->pixels[y - 1] != 0xFF)
                column->pixels[y] = prevColumn->pixels[y];  // copy the color from the left
            else
                column->pixels[y] = column->pixels[y - 1];  // copy the color from above
        }
    }

    free(countsInColumn);
}

void R_InitPatches(void)
{
    patches = calloc(numlumps, sizeof(rpatch_t));

    texture_composites = calloc(numtextures, sizeof(rpatch_t));

    BIGDOOR7 = R_CheckTextureNumForName("BIGDOOR7");
    FIREBLU1 = R_CheckTextureNumForName("FIREBLU1");
    SKY1 = R_CheckTextureNumForName("SKY1");
    STEP2 = R_CheckTextureNumForName("STEP2");

    for (int i = 0; i < numspritelumps; i++)
        createPatch(firstspritelump + i);

    for (int i = 0; i < numtextures; i++)
        createTextureCompositePatch(i);
}

const rpatch_t *R_CachePatchNum(int id)
{
    return &patches[id];
}

const rpatch_t *R_CacheTextureCompositePatchNum(int id)
{
    return &texture_composites[id];
}

const rcolumn_t *R_GetPatchColumnWrapped(const rpatch_t *patch, int columnIndex)
{
    while (columnIndex < 0)
        columnIndex += patch->width;

    return &patch->columns[columnIndex % patch->width];
}

const rcolumn_t *R_GetPatchColumnClamped(const rpatch_t *patch, int columnIndex)
{
    return &patch->columns[BETWEEN(0, columnIndex, patch->width - 1)];
}
