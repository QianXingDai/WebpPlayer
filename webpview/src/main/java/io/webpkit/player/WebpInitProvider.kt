package io.webpkit.player

import android.content.ContentProvider
import android.content.ContentValues
import android.database.Cursor
import android.net.Uri

/**
 * No-op [ContentProvider] used solely to capture the application context at process
 * startup (ContentProviders are created before Application.onCreate). This gives the
 * library zero-config initialization — consumers never have to call an init method.
 */
class WebpInitProvider : ContentProvider() {

    override fun onCreate(): Boolean {
        context?.let { WebpContext.init(it) }
        return true
    }

    override fun query(
        uri: Uri,
        projection: Array<out String>?,
        selection: String?,
        selectionArgs: Array<out String>?,
        sortOrder: String?,
    ): Cursor? = null

    override fun getType(uri: Uri): String? = null

    override fun insert(uri: Uri, values: ContentValues?): Uri? = null

    override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?): Int = 0

    override fun update(
        uri: Uri,
        values: ContentValues?,
        selection: String?,
        selectionArgs: Array<out String>?,
    ): Int = 0
}
