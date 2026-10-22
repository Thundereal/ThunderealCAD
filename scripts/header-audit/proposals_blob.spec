# Run with --no-iwyu: the split leaves the blob value types (FileBlob, BlobReferrer...) in a
# new App/FileBlob.h that the properties include; this measures who still reaches the manager.
name: S01 FileBlobManager class in its own header, value types in FileBlob.h
cut: src/App/AppearanceList.h -> src/App/FileBlobManager.h
cut: src/App/PropertyStandard.h -> src/App/FileBlobManager.h
cut: src/App/FileSet.h -> src/App/FileBlobManager.h
cut: src/App/PropertyFile.h -> src/App/FileBlobManager.h
cut: src/App/PropertyElementAppearance.h -> src/App/FileBlobManager.h
cut: src/App/MaterialXDocument.h -> src/App/FileBlobManager.h
cut: src/Mod/Material/App/PropertyMaterial.h -> src/App/FileBlobManager.h
cut: src/Mod/Part/App/PropertyTopoShape.h -> src/App/FileBlobManager.h
cut: src/App/PropertyHistory.h -> src/App/FileBlobManager.h
needs: \bFileBlobManager\b|getBlobManager|blobManager
