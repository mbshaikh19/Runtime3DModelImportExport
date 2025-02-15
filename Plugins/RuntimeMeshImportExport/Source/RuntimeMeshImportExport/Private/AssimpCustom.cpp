// MIT License
//
// Copyright (c) 2019 Lucid Layers

#include "AssimpCustom.h"
#include "Async/Async.h"
#include "RuntimeMeshImportExport.h"
#include "RuntimeMeshImportExportLibrary.h"
#include "RuntimeMeshImportExportTypes.h"
//#include "C:/Program Files/Epic Games/UE_4.25/Engine/Source/Runtime/ImageWriteQueue/Public/ImageWriteBlueprintLibrary.h"
#include "Exporters/TextureExporterTGA.h"
#include "Exporters/TextureExporterBMP.h"
#include "Materials/MaterialExpressionConstant.h"

#include "ProfilingDebugging/ScopedTimers.h"

FAssimpScene::FAssimpScene()
{
    rootNode = new FAssimpNode(FName(), nullptr);
}

FAssimpScene::~FAssimpScene()
{
    ClearParentDataAndPtrs();
    ClearMeshData();

    delete rootNode;
}

void FAssimpScene::SetDataAndPtrsToParentClass_EntireScene(const FRuntimeMeshExportParam& param)
{
    mNumMeshes = meshes.Num();
    mMeshes = (aiMesh**)meshes.GetData();
    mNumMaterials = materials.Num();
    mMaterials = materials.GetData();

    check(rootNode);
    mRootNode = (aiNode*)rootNode;
    rootNode->SetDataAndPtrsToParentClass(param);

    for (FAssimpMesh* mesh : meshes)
    {
        mesh->SetDataAndPtrsToParentClass(param);
    }
}

FAssimpMesh::~FAssimpMesh()
{
    // Remove the parent references to the data
    mNumVertices = 0;
    mNumFaces = 0;
    mVertices = nullptr;
    mNormals = nullptr;
    mTangents = nullptr;
    mBitangents = nullptr;
    for (unsigned int a = 0; a < AI_MAX_NUMBER_OF_COLOR_SETS; a++) {
        mColors[a] = nullptr;
    }
    for (unsigned int a = 0; a < AI_MAX_NUMBER_OF_TEXTURECOORDS; a++) {
        mTextureCoords[a] = nullptr;
    }
    mFaces = nullptr;
    mBones = nullptr;
    mAnimMeshes = nullptr;

    for (aiFace& face : faces)
    {
        delete[] face.mIndices;
        face.mIndices = nullptr;
        face.mNumIndices = 0;
    }
}

void FAssimpMesh::SetDataAndPtrsToParentClass(const FRuntimeMeshExportParam& param)
{
    mNumVertices = vertices.Num();
    mVertices = vertices.GetData();
    mNormals = normals.GetData();
    mTangents = tangents.GetData();
    mBitangents = bitangents.GetData();
    mColors[0] = vertexColors.GetData();
    for (uint32 i = 0; i < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++i)
    {
        mNumUVComponents[i] = numUVComponents[i];
        mTextureCoords[i] = textureCoordinates[i].GetData();
    }
    mNumFaces = faces.Num();
    mFaces = (aiFace*)faces.GetData();
}

FAssimpNode::~FAssimpNode()
{
    ClearParentDataAndPtrs();
    ClearMeshData();

    for (FAssimpNode* child : children)
    {
        delete child;
    }
    // Free our own data
    parent = nullptr;
}

void FAssimpNode::SetDataAndPtrsToParentClass(const FRuntimeMeshExportParam& param)
{
    // Set the name of the node
    mName.Set(TCHAR_TO_ANSI(*name.ToString()));

    // Set the transform for the node
    FTransform relativeTransform = worldTransform;
    if (parent) 
    {
		// If has a parent
        relativeTransform = parent->worldTransform.Inverse() * worldTransform;
    }
    else
    {
		// Is parent

        // Apply transform corrections
        FVector scale = relativeTransform.GetScale3D();
        scale *= param.correction.scaleFactor;
        if (param.correction.bFlipX)
        {
            scale.X *= -1.f;
        }
        if (param.correction.bFlipY)
        {
            scale.Y *= -1.f;
        }
        if (param.correction.bFlipZ)
        {
            scale.Z *= -1.f;
        }
        relativeTransform.SetScale3D(scale);

        FRotator deltaRot(URuntimeMeshImportExportLibrary::RotationCorrectionToValue(param.correction.PitchCorrection_Y)
                          , URuntimeMeshImportExportLibrary::RotationCorrectionToValue(param.correction.YawCorrection_Z)
                          , URuntimeMeshImportExportLibrary::RotationCorrectionToValue(param.correction.RollCorrection_X));
        relativeTransform.ConcatenateRotation(deltaRot.Quaternion());
    }
    mTransformation = URuntimeMeshImportExportLibrary::FTransformToAiTransform(relativeTransform);

    mNumMeshes = meshRefIndices.Num();
    mMeshes = (unsigned int*)meshRefIndices.GetData();
    mNumChildren = children.Num();
    mChildren = (aiNode**)children.GetData();

    for (FAssimpNode* child : children)
    {
        child->SetDataAndPtrsToParentClass(param);
    }
}


void FAssimpNode::ClearParentDataAndPtrs()
{
    mNumChildren = 0;
    mChildren = nullptr;
    mNumMeshes = 0;
    mMeshes = nullptr;
    mMetaData = nullptr;
}

void FAssimpNode::ClearMeshData()
{
    indexGatherNext = 0;
    gatheredExportables.Empty();
    meshRefIndices.Empty();
}

FString FAssimpNode::GetHierarchicalName() const
{
    if (parent)
    {
        return parent->GetHierarchicalName() + TEXT(".") + name.ToString();
    }
    else
    {
        return FString(TEXT("root"));
    }
}

FAssimpNode* FAssimpNode::FindOrCreateNode(TArray<FString>& nodePathRelative)
{
    if (nodePathRelative.Num() > 0)
    {
        FName nodeName = FName(*MoveTemp(nodePathRelative[0]));
        nodePathRelative.RemoveAt(0);

        FAssimpNode** foundNode = children.FindByPredicate([nodeName](FAssimpNode* child) -> bool {
            return child->name == nodeName;
        });

        FAssimpNode* childNode = nullptr;
        if (foundNode)
        {
            childNode = *foundNode;
        }
        else
        {
            childNode = children.Add_GetRef(new FAssimpNode(nodeName, this));
        }

        check(childNode);
        return childNode->FindOrCreateNode(nodePathRelative);
    }

    return this;
}

void FAssimpNode::ClearExportData()
{
    ClearParentDataAndPtrs();
    ClearMeshData();
    for (FAssimpNode* child : children)
    {
        child->ClearExportData();
    }
}

void FAssimpNode::GetNodesRecursive(TArray<FAssimpNode*>& outNodes)
{
    outNodes.Add(this);
    for (FAssimpNode* child : children)
    {
        if (child)
        {
            child->GetNodesRecursive(outNodes);
        }
    }
}

int32 FAssimpNode::GatherMeshData(FAssimpScene& scene, const FRuntimeMeshExportParam& param, const bool bGatherAll, const int32 numToGather)
{
    check(IsInGameThread());
    check(bGatherAll || numToGather > 0);

    int32 numGathered = 0;
    const int32 startIndex = bGatherAll ? 0 : indexGatherNext;
    const int32 endIndex = FMath::Min(bGatherAll ? exportObjects.Num() : startIndex + numToGather, exportObjects.Num());
    for (; indexGatherNext < endIndex; ++indexGatherNext)
    {
        ++numGathered;

        TScriptInterface<IMeshExportable>& object = exportObjects[indexGatherNext];
        TArray<FExportableMeshSection> sections;
        if (!object->Execute_GetMeshData(object.GetObject(), param.lod, param.bSkipLodNotValid, sections))
        {
            scene.WriteToLogWithNewLine(FString::Printf(TEXT("Object %s refused to be part of export."), *object.GetObject()->GetName()));
            ++scene.numObjectsSkipped;
            continue;
        }

        if (sections.Num() == 0)
        {
            scene.WriteToLogWithNewLine(FString::Printf(TEXT("Object %s did not return any sections."), *object.GetObject()->GetName()));
            ++scene.numObjectsSkipped;
            continue;
        }

        // Validate all sections
        {
            bool bAllSectionsValid = true;
            for (int32 sectionIndex = sections.Num() - 1; sectionIndex >= 0; --sectionIndex)
            {
                if (!ValidateMeshSection(scene, object, sections[sectionIndex]))
                {
                    scene.WriteToLogWithNewLine(FString::Printf(TEXT("Object %s: Section %d failed validation."), *object.GetObject()->GetName(), sectionIndex));
                    bAllSectionsValid = false;
                }
            }

            if (!bAllSectionsValid)
            {
                scene.WriteToLogWithNewLine(FString::Printf(TEXT("Object %s has invalid sections. Skipped."), *object.GetObject()->GetName()));
                ++scene.numObjectsSkipped;
                continue;
            }
        }

        gatheredExportables.Add(sections);
    }

    return numGathered;
}

void FAssimpNode::ProcessGatheredData_Recursive(FAssimpScene& scene, const FRuntimeMeshExportParam& param)
{
    check(!parent); // should only be called on the root node
    scene.WriteToLogWithNewLine(FString(TEXT("Begin processing gathered data.")));
    double duration = 0.f;
    {
        FScopedDurationTimer timer(duration);
        ProcessGatheredData_Internal(scene, param);
    }
    scene.WriteToLogWithNewLine(FString::Printf(TEXT("End processing gathered data. Duration: %.3fs"), duration));
}

void FAssimpNode::ProcessGatheredData_Internal(FAssimpScene& scene, const FRuntimeMeshExportParam& param)
{
    FString hierarchicalName = GetHierarchicalName();

    // Create meshes
    CreateAssimpMeshesFromMeshData(scene, param);
    scene.WriteToLogWithNewLine(FString::Printf(TEXT("Node %s has %d meshes for export."), *hierarchicalName, meshRefIndices.Num()));

    // Process children
    for (int32 childIndex = children.Num() - 1; childIndex >= 0; --childIndex)
    {
        children[childIndex]->ProcessGatheredData_Internal(scene, param);
    }

    if (children.Num() == 0 && meshRefIndices.Num() == 0)
    {
        scene.WriteToLogWithNewLine(FString::Printf(TEXT("Node %s has no children and no meshes."), *hierarchicalName));
    }
}


void FAssimpNode::CreateAssimpMeshesFromMeshData(FAssimpScene& scene, const FRuntimeMeshExportParam& param)
{
    // Process the gathered mesh data
    TMap<UMaterialInterface*, TArray<FExportableMeshSection>> mapMaterialSections;
    UE_LOG(LogTemp,Warning,TEXT(" oooo gathered sections = %d"), gatheredExportables.Num());
    for(TArray<FExportableMeshSection>& sections : gatheredExportables)
    {
        // Transform the data
        for (FExportableMeshSection& section : sections)
        {
            FTransform objectSpaceToNodeSpace = section.meshToWorld * this->worldTransform.Inverse();
            for (int32 i = section.vertices.Num() -1 ; i >= 0; --i)
            {
                section.vertices[i] = objectSpaceToNodeSpace.TransformPosition(section.vertices[i]);
                section.normals[i] = objectSpaceToNodeSpace.TransformVector(section.normals[i]);
                section.tangents[i] = objectSpaceToNodeSpace.TransformVector(section.tangents[i]);              
            }

            TArray<FExportableMeshSection>& materialSections = mapMaterialSections.FindOrAdd(section.material);

            // Combine data of the same material if wanted
            if (param.bCombineSameMaterial && materialSections.IsValidIndex(0))
            {
                materialSections[0].Append(MoveTemp(section));
            }
            else
            {
                materialSections.Add(MoveTemp(section));
            }
        }
    }
    gatheredExportables.Empty();

    // Create aiMeshes from the gathered data
    for (auto& element : mapMaterialSections)
    {
        for (FExportableMeshSection& section : element.Value)
        {            
            // Create the aiMesh
            FAssimpMesh* mesh = new FAssimpMesh();
            meshRefIndices.Add(scene.meshes.Add(mesh));

            // mesh->mName = TODO do we need a name for the meshes?! Problem with merged meshes
            mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;

            // Add the material to the mesh
			const int32 foundMaterialIndex = scene.uniqueMaterials.Find(section.material);
            //scene.WriteToLogWithNewLine(FString::Printf(TEXT("oooo mesh material index = %d"), foundMaterialIndex));
			if (foundMaterialIndex == INDEX_NONE)
			{                
                aiString assimpTexturePath;
                assimpTexturePath = "E:\\Unreal Engine Projects\\ImportExportDemo\\Export\\T_Chair_M.jpg";
				mesh->mMaterialIndex = scene.uniqueMaterials.Add(section.material);
				aiMaterial* material = new aiMaterial();
				scene.materials.Add(material);
				check(scene.uniqueMaterials.Num() == scene.materials.Num())
				// Set the material name
				{                    
					aiString materialName;
					if (section.material)
					{
						materialName = TCHAR_TO_ANSI(*section.material->GetName());
					}
					else
					{
						materialName = "Unknown";
					}
					material->AddProperty(&materialName, AI_MATKEY_NAME);
				}
				// Set the material to be two sided
				{
					const int bTwoSided = true;
					material->AddProperty(&bTwoSided, 1, AI_MATKEY_TWOSIDED);
				}
				// Shininess (only a FIX for gltf.v1 crash cause shininess not available)
				{
					const float shininess = 1.0f;
					material->AddProperty(&shininess, 1, AI_MATKEY_SHININESS);
				}

                //Export Normal texture and add in aiMaterial - begin
                TArray<UTexture*> OutTextures;
                TArray<TArray<int32>> OutIndices;
                section.material->GetUsedTexturesAndIndices(OutTextures,OutIndices,EMaterialQualityLevel::Type::High,ERHIFeatureLevel::SM5);
                UE_LOG(LogTemp, Display, TEXT("M_M Total texture in material = %d"), OutTextures.Num()); 
                for (UTexture* currentTex : OutTextures)
                {   
                    FString FileName;
                    ExportTexture(currentTex, FileName);
                    UE_LOG(LogTemp, Display, TEXT("M_M Assimp Exporting mesh..."));
                                       
                    //convert FString texture path into aiString for assimp
                    aiString assimpTexPath;
                    assimpTexPath = FStringToaiString(FileName);
                    
                    //FTextureFormatSettings contains texture details
                    FTextureFormatSettings texFormatSetting;
                    currentTex->GetDefaultFormatSettings(texFormatSetting);

                    //Check for Normal texture
                    if (texFormatSetting.CompressionSettings == TextureCompressionSettings::TC_Normalmap)
                    {
                        //Add texture as a property to material being exported
                        material->AddProperty(&assimpTexPath,AI_MATKEY_TEXTURE_NORMALS(0));
                    }
                }
                //Export Normal texture and add into aiMaterial - end

                //Read metallic parameter and add into aiMaterial
                {
                    FHashedMaterialParameterInfo matOpaqueParamInfo;
                    UTexture* diffuse_tex;
                    matOpaqueParamInfo.Name = FName("DiffuseMap");                    
                    if (section.material->GetTextureParameterValue(matOpaqueParamInfo, diffuse_tex))
                    {
                        FString FileName;
                        ExportTexture(diffuse_tex, FileName);
                        aiString str = FStringToaiString(diffuse_tex->GetFName().ToString() + ".bmp");
                        material->AddProperty(&str, AI_MATKEY_TEXTURE_DIFFUSE(0));
                    }
                }

                {
                    FHashedMaterialParameterInfo matOpaqueParamInfo;
                    UTexture* emissive_tex;
                    matOpaqueParamInfo.Name = FName("EmissiveMap");
                    if (section.material->GetTextureParameterValue(matOpaqueParamInfo, emissive_tex))
                    {
                        FString FileName;
                        ExportTexture(emissive_tex, FileName);
                        aiString str = FStringToaiString(emissive_tex->GetFName().ToString() + ".bmp");
                        material->AddProperty(&str, AI_MATKEY_TEXTURE_EMISSIVE(0));
                    }
                }

                {
                    FHashedMaterialParameterInfo matOpaqueParamInfo;
                    UTexture* ao_tex;
                    matOpaqueParamInfo.Name = FName("AOMap");
                    if (section.material->GetTextureParameterValue(matOpaqueParamInfo, ao_tex))
                    {
                        FString FileName;
                        ExportTexture(ao_tex, FileName);
                        aiString str = FStringToaiString(ao_tex->GetFName().ToString() + ".bmp");
                        material->AddProperty(&str, AI_MATKEY_TEXTURE_LIGHTMAP(0));
                    }
                }

                {
                    FHashedMaterialParameterInfo matOpaqueParamInfo;
                    UTexture* opacity_tex;
                    matOpaqueParamInfo.Name = FName("OpacityMap");
                    if (section.material->GetTextureParameterValue(matOpaqueParamInfo, opacity_tex))
                    {
                        FString FileName;
                        ExportTexture(opacity_tex, FileName);
                        aiString str = FStringToaiString(opacity_tex->GetFName().ToString() + ".bmp");
                        material->AddProperty(&str, AI_MATKEY_TEXTURE_OPACITY(0));
                    }
                }

                {
                    FHashedMaterialParameterInfo matOpaqueParamInfo;
                    UTexture* metallic_tex;
                    matOpaqueParamInfo.Name = FName("MetallicMap");
                    if (section.material->GetTextureParameterValue(matOpaqueParamInfo, metallic_tex))
                    {
                        FString FileName;
                        ExportTexture(metallic_tex, FileName);
                        aiString str = FStringToaiString(metallic_tex->GetFName().ToString() + ".bmp");
                        float mettalicFactor = 0.8f;
                        material->AddProperty(&str, AI_MATKEY_GLTF_PBRMETALLICROUGHNESS_METALLICROUGHNESS_TEXTURE);
                    }
                }

                //Vector material parameters
                {
                    FHashedMaterialParameterInfo color4DBasecolorParam;
                    FLinearColor basecolorValue;
                    color4DBasecolorParam.Name = FName("BaseColor");
                    if (section.material->GetVectorParameterValue(color4DBasecolorParam, basecolorValue))
                    {
                        aiColor3D transColor(basecolorValue.R, basecolorValue.G, basecolorValue.B);
                        material->AddProperty(&transColor, 1, AI_MATKEY_COLOR_DIFFUSE);
                    }
                }
                
                {
                    FHashedMaterialParameterInfo color4DSpecularParam;
                    FLinearColor specColor;
                    color4DSpecularParam.Name = FName("SpecularColor");
                    if (section.material->GetVectorParameterValue(color4DSpecularParam, specColor))
                    {
                        aiColor4D specularColor(specColor.R, specColor.G, specColor.B,specColor.A);
                        material->AddProperty(&specularColor, 1, AI_MATKEY_GLTF_PBRSPECULARGLOSSINESS_SPECULAR_FACTOR);
                    }
                }
                  
                {
                    FHashedMaterialParameterInfo color4DSpecularParam;
                    FLinearColor emissiveColor;
                    color4DSpecularParam.Name = FName("EmissiveColor");
                    if (section.material->GetVectorParameterValue(color4DSpecularParam, emissiveColor))
                    {
                        aiColor4D specularColor(emissiveColor.R, emissiveColor.G, emissiveColor.B, emissiveColor.A);
                        material->AddProperty(&specularColor, 1, AI_MATKEY_COLOR_EMISSIVE);
                    }
                }

                //Scalar material parameters
                {
                    FHashedMaterialParameterInfo constRoughnessParam;
                    constRoughnessParam.Name = FName("Roughness_strength");
                    float roughnessValue=0.0f;
                    if (section.material->GetScalarParameterValue(constRoughnessParam, roughnessValue))
                    {
                        material->AddProperty(&roughnessValue, 1, AI_MATKEY_GLTF_PBRMETALLICROUGHNESS_ROUGHNESS_FACTOR);
                    }
                }

                {
                    FHashedMaterialParameterInfo constMetallicParam;
                    constMetallicParam.Name = FName("Metallic_strength");
                    float MetallicValue = 0.0f;
                    if (section.material->GetScalarParameterValue(constMetallicParam, MetallicValue))
                    {
                        material->AddProperty(&MetallicValue, 1, AI_MATKEY_GLTF_PBRMETALLICROUGHNESS_METALLIC_FACTOR);
                    }
                }

                {
                    FHashedMaterialParameterInfo constRefractionParam;
                    constRefractionParam.Name = FName("Refraction_strength");
                    float refractionValue = 0.0f;      
                    if (section.material->GetScalarParameterValue(constRefractionParam, refractionValue))
                    {
                        material->AddProperty(&refractionValue, 1, AI_MATKEY_REFRACTI);
                    }
                }

                {
                    FHashedMaterialParameterInfo constOpacityParam;
                    constOpacityParam.Name = FName("Opacity_strength");
                    float opacityValue = 1;
                    if (section.material->GetScalarParameterValue(constOpacityParam, opacityValue))
                    {
                        material->AddProperty(&opacityValue, 1, AI_MATKEY_OPACITY);
                    }
                }

                //experiment
                /*{
                    float matProperty = 0.85, property2 = 0.8, roughValue = 0.1f;
                    //material->AddProperty(&matProperty,1,AI_MATKEY_REFLECTIVITY);
                    aiColor4D property3(0.45, 0.45, 0.45, 0.5);
                    aiColor3D specularColor(0.69, 0.69, 0.69);
                    //material->AddProperty(&specularColor, 1, AI_MATKEY_COLOR_SPECULAR);
                    //material->AddProperty<aiColor4D>(&property3, 1, AI_MATKEY_COLOR_REFLECTIVE);
                    material->AddProperty(&property2, 1, AI_MATKEY_GLTF_PBRMETALLICROUGHNESS_METALLIC_FACTOR);

                }*/
                
                if (section.material->GetBlendMode() == EBlendMode::BLEND_Translucent)
                {

                }
                else if (section.material->GetBlendMode() == EBlendMode::BLEND_Opaque)
                {
                    
                }
			}
			else
			{
				mesh->mMaterialIndex = foundMaterialIndex;
			}

            // Vertices
            {
                // Do some checks to make sure we move the data
                check(sizeof(aiVector3D) == sizeof(FVector));
                check(sizeof(aiColor4D) == sizeof(FLinearColor));

                int32 numVertices = section.vertices.Num();

                // Positions
                mesh->vertices = MoveTemp(*reinterpret_cast<TArray<aiVector3D>*>(&section.vertices));
                check(section.vertices.Num() == 0); // just to check that the move worked

                // Normals
                mesh->normals = MoveTemp(*reinterpret_cast<TArray<aiVector3D>*>(&section.normals));

                // Tangents
                mesh->tangents = MoveTemp(*reinterpret_cast<TArray<aiVector3D>*>(&section.tangents));

                // Bitangents
                // Seems that Assimp requires bitangents, though we do not supply values for now. See how it works out.
                mesh->bitangents.AddZeroed(numVertices);

                // Colors
                TArray<FLinearColor> linearColors;
                linearColors.Reserve(numVertices);
                for (int32 colorIndex = 0; colorIndex < numVertices; ++colorIndex)
                {
                    linearColors.Add(section.vertexColors[colorIndex].ReinterpretAsLinear());
                }
                mesh->vertexColors = MoveTemp(*reinterpret_cast<TArray<aiColor4D>*>(&linearColors));


                // TextureCoordinates
                mesh->numUVComponents[0] = 2;
                mesh->textureCoordinates->Reserve(numVertices);
                for (int32 texIndex = 0; texIndex < numVertices; ++texIndex)
                {
                    FVector2D& coord = section.textureCoordinates[texIndex];
                    mesh->textureCoordinates[0].Add(aiVector3D(coord.X, coord.Y, 0.f));
                }
            }

            // Faces
            {
                const int32 numIndicesPerFace = 3;
                check((section.triangles.Num() % numIndicesPerFace) == 0);
                const int32 numFaces = section.triangles.Num() / numIndicesPerFace;
                mesh->faces.SetNum(numFaces);
                for (int32 faceIndex = 0; faceIndex < numFaces; ++faceIndex)
                {
                    int32 triangleStart = faceIndex * numIndicesPerFace;
                    aiFace& face = mesh->faces[faceIndex];
                    face.mNumIndices = numIndicesPerFace;
                    face.mIndices = new unsigned int[numIndicesPerFace];
                    face.mIndices[0] = section.triangles[triangleStart];
                    face.mIndices[1] = section.triangles[triangleStart + 1];
                    face.mIndices[2] = section.triangles[triangleStart + 2];
                }
            }
        }
    }
}


bool FAssimpNode::ValidateMeshSection(FAssimpScene& scene, TScriptInterface<IMeshExportable>& exportable, FExportableMeshSection& section)
{
    bool bMeshValid = true;
    int32 numVertices = section.vertices.Num();
    if (section.normals.Num() != numVertices)
    {
        scene.WriteToLogWithNewLine(FString::Printf(TEXT("Object %: Number of normals not equal number of vertices!"), *exportable.GetObject()->GetName()));
        bMeshValid = false;
    }

    if (section.tangents.Num() != numVertices)
    {
        scene.WriteToLogWithNewLine(FString::Printf(TEXT("Object %: Number of tangents not equal number of vertices!"), *exportable.GetObject()->GetName()));
        bMeshValid = false;
    }

    if (section.vertexColors.Num() != numVertices)
    {
        scene.WriteToLogWithNewLine(FString::Printf(TEXT("Object %: Number of vertexColors not equal number of vertices!"), *exportable.GetObject()->GetName()));
        bMeshValid = false;
    }

    if (section.textureCoordinates.Num() != numVertices)
    {
        scene.WriteToLogWithNewLine(FString::Printf(TEXT("Object %: Number of textureCoordinates not equal number of vertices!"), *exportable.GetObject()->GetName()));
        bMeshValid = false;
    }

    if (section.triangles.Num() % 3 != 0)
    {
        scene.WriteToLogWithNewLine(FString::Printf(TEXT("Object %: Number of triangles is not dividable by 3!"), *exportable.GetObject()->GetName()));
        bMeshValid = false;
    }

    return bMeshValid;
}


void FAssimpScene::WriteToLogWithNewLine(const FString& logText)
{
    if (exportLog)
    {
        URuntimeMeshImportExportLibrary::NewLineAndAppend(*exportLog, logText);
    }

    if (bLogToUnreal)
    {
        RMIE_LOG(Log, "%s", *logText);
    }
}

void FAssimpScene::PrepareSceneForExport(const FRuntimeMeshExportParam& param)
{
    numObjectsSkipped = 0;
	WriteToLogWithNewLine(FString(TEXT("Begin gather mesh data.")));
    rootNode->GetNodesRecursive(allNodesHelper);
	double duration = 0.f;
	{
		FScopedDurationTimer timer(duration);
		for (FAssimpNode* node : allNodesHelper)
		{
			node->GatherMeshData(*this, param, true);
		}
	}
	WriteToLogWithNewLine(FString::Printf(TEXT("End gather mesh data. Duration: %.3fs"), duration));

    rootNode->ProcessGatheredData_Recursive(*this, param);
    SetDataAndPtrsToParentClass_EntireScene(param);
}

void FAssimpScene::PrepareSceneForExport_Async_Start(const FRuntimeMeshExportAsyncParam& param, FRuntimeMeshImportExportProgressUpdate callbackProgress
        , TFunction<void()> onPrepareFinished)
{
    numObjectsSkipped = 0;
    gatheredMeshNum = 0;
    currentNodeIndex = 0;
    delegateProgress = callbackProgress;
    onGameThreadPrepareFinished = onPrepareFinished;
	numGatherPerTick = param.numGatherPerTick < 1 ? 1 : param.numGatherPerTick;
    rootNode->GetNodesRecursive(allNodesHelper);
	startTimeGatherMeshData = FPlatformTime::Seconds();
	WriteToLogWithNewLine(FString(TEXT("Begin gather mesh data.")));
    gatherMeshDataTicker = MakeUnique<FGatherMeshDataTicker>(this, param);
}

void FAssimpScene::PrepareSceneForExport_Update(const FRuntimeMeshExportParam& param)
{
    check(IsInGameThread());
    check(numGatherPerTick > 0);
    if (currentNodeIndex >= allNodesHelper.Num())
    {
        // Just a precaution in case the ticker is called another time, though the gathering is finished
        checkNoEntry();
    }

    const int32 endIndex = currentNodeIndex + numGatherPerTick;
    int32 numToGather = numGatherPerTick;
    while (numToGather)
    {
        int32 numGathered = allNodesHelper[currentNodeIndex]->GatherMeshData(*this, param, false, numToGather);
        gatheredMeshNum += numGathered;
        if (numGathered == 0)
        {
            ++currentNodeIndex;
            if (currentNodeIndex >= allNodesHelper.Num())
            {
                // We are inside the function that is ticked from the ticker, can't reset the ticker from here.
                AsyncTask(ENamedThreads::GameThread, [this]() {
					if (this)
					{
						gatherMeshDataTicker.Reset();
					}
                });

				WriteToLogWithNewLine(FString::Printf(TEXT("End gather mesh data. Duration: %.3fs"), FPlatformTime::Seconds() - startTimeGatherMeshData));

                onGameThreadPrepareFinished();
                break;
            }
        }
        numToGather -= numGathered;
    }

    //delegateStatus.ExecuteIfBound(FString::Printf(TEXT("Gathering exportables in %d/%d nodes. Meshes: %d "), currentNodeIndex + 1, allNodesHelper.Num(), gatheredMeshNum));
	delegateProgress.ExecuteIfBound(FRuntimeMeshImportExportProgress(ERuntimeMeshImportExportProgressType::GatheringMeshs, currentNodeIndex, allNodesHelper.Num()));
}

void FAssimpScene::PrepareSceneForExport_Async_Finish(const FRuntimeMeshExportParam& param)
{
    check(!IsInGameThread());
  //  AsyncTask(ENamedThreads::GameThread, [this]() {
		//delegateStatus.ExecuteIfBound(FString::Printf(TEXT("Processing gathered data")));
  //  });

    rootNode->ProcessGatheredData_Recursive(*this, param);

  //  AsyncTask(ENamedThreads::GameThread, [this]() {
		//delegateStatus.ExecuteIfBound(FString::Printf(TEXT("Giving Assimp types data access")));
  //  });
    SetDataAndPtrsToParentClass_EntireScene(param);
}

void FAssimpScene::ClearSceneExportData()
{
	delegateProgress.Unbind();
    currentNodeIndex = 0;
    numGatherPerTick = -1;

    ClearParentDataAndPtrs();
    ClearMeshData();
    rootNode->ClearExportData();
}

void FAssimpScene::ClearParentDataAndPtrs()
{
    mRootNode = nullptr;
    mMeshes = nullptr;
    mNumMeshes = 0;
    mMaterials = nullptr;
    mNumMaterials = 0;
    mAnimations = nullptr;
    mNumAnimations = 0;
    mTextures = nullptr;
    mNumTextures = 0;
    mLights = nullptr;
    mNumLights = 0;
    mCameras = nullptr;
    mNumCameras = 0;
}

void FAssimpScene::ClearMeshData()
{
	uniqueMaterials.Empty();

	for (FAssimpMesh* mesh : meshes)
    {
        delete mesh;
    }
    meshes.Empty();

    for (aiMaterial* material : materials)
    {
        delete material;
    }
    materials.Empty();
}

bool FAssimpNode::ExportTexture(UTexture* textureRef,FString &outTexturePath)
{
    if (textureRef != nullptr)
    {
        bool isExported;
        outTexturePath = FPaths::ProjectDir() + "Export/" + textureRef->GetFName().ToString() + ".bmp";
        // Exports texture
        isExported = UExporter::ExportToFile(textureRef, NULL, *outTexturePath, false) >=1 ? true : false;
        UE_LOG(LogTemp, Display, TEXT("M_M exported texture name = %s  is successfully exported? = %d"), *outTexturePath, isExported);
        return isExported;
        
    }
    else
        return false;
}

aiString FAssimpNode::FStringToaiString(FString str)
{
    aiString assimpString;
    assimpString=std::string(TCHAR_TO_UTF8(*str));
    return assimpString;
}